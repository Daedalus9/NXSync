#include "nxsync/preflight_protocol.hpp"
#include "nxsync/atomic_file.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace nxsync {
namespace {

bool isHex(const std::string& value, const std::size_t length) {
    return value.size() == length
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

std::string escapeValue(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\') result += "\\\\";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '=') result += "\\e";
        else result.push_back(ch);
    }
    return result;
}

bool unescapeValue(const std::string& value, std::string& output) {
    output.clear();
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') escaped = true;
        else if (escaped) {
            if (ch == '\\') output.push_back('\\');
            else if (ch == 'n') output.push_back('\n');
            else if (ch == 'r') output.push_back('\r');
            else if (ch == 'e') output.push_back('=');
            else return false;
            escaped = false;
        } else output.push_back(ch);
    }
    return !escaped;
}

bool parseUnsigned(const std::string& value, std::uint64_t& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool writeAtomic(const std::string& path, const std::string& text, int& systemError) {
    return writeTextFileAtomic(path, text, systemError);
}

bool validRequest(const PreflightRequest& request, std::string& error) {
    if (request.version != PreflightProtocolVersion || request.sequence == 0
        || !isHex(request.titleId, 16)
        || (!request.automaticLaunch && !isHex(request.profileUid, 32))
        || (request.automaticLaunch && !request.profileUid.empty()
            && !isHex(request.profileUid, 32))) {
        error = "Invalid preflight request";
        return false;
    }
    error.clear();
    return true;
}

bool validStatus(const PreflightStatus& status, std::string& error) {
    if (status.version != PreflightProtocolVersion
        || status.workerBuildVersion.empty() || status.sequence == 0
        || !isHex(status.titleId, 16)
        || (!status.profileUid.empty() && !isHex(status.profileUid, 32))
        || (status.state != "working" && status.state != "completed"
            && status.state != "error")) {
        error = "Invalid preflight status";
        return false;
    }
    if (status.state == "completed" && status.outcome.empty()) {
        error = "Preflight outcome is missing";
        return false;
    }
    if (status.candidates.size() > 128) {
        error = "Too many cloud candidates in preflight status";
        return false;
    }
    if ((!status.localRevisionId.empty()
            || !status.localPayloadSha256.empty())
        && (!isHex(status.localRevisionId, 64)
            || !isHex(status.localPayloadSha256, 64))) {
        error = "Invalid local revision in preflight status";
        return false;
    }
    if (!status.selectedRevisionId.empty()
        && !status.selectedPayloadSha256.empty()
        && !isHex(status.selectedPayloadSha256, 64)) {
        error = "Invalid selected payload in preflight status";
        return false;
    }
    for (const PreflightCandidate& candidate : status.candidates) {
        if (!isHex(candidate.revisionId, 64)
            || !isHex(candidate.payloadSha256, 64)
            || candidate.archivePath.empty()) {
            error = "Invalid cloud candidate in preflight status";
            return false;
        }
    }
    error.clear();
    return true;
}

} // namespace

std::string serializePreflightRequest(const PreflightRequest& request) {
    return "version=" + std::to_string(request.version) + "\n"
        + "sequence=" + std::to_string(request.sequence) + "\n"
        + "title_id=" + request.titleId + "\n"
        + "profile_uid=" + request.profileUid + "\n"
        + "automatic_launch="
            + std::string(request.automaticLaunch ? "true" : "false") + "\n";
}

bool parsePreflightRequest(
    const std::string& text,
    PreflightRequest& request,
    std::string& error) {
    request = PreflightRequest{};
    bool versionSeen = false;
    bool sequenceSeen = false;
    bool titleSeen = false;
    bool profileSeen = false;
    bool automaticSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            const std::string value = line.substr(separator + 1);
            std::uint64_t number = 0;
            if (key == "version" && parseUnsigned(value, number)) {
                request.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "sequence" && parseUnsigned(value, number)) {
                request.sequence = number;
                sequenceSeen = true;
            } else if (key == "title_id") {
                request.titleId = value;
                titleSeen = true;
            } else if (key == "profile_uid") {
                request.profileUid = value;
                profileSeen = true;
            } else if (key == "automatic_launch"
                && (value == "true" || value == "false")) {
                request.automaticLaunch = value == "true";
                automaticSeen = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !sequenceSeen || !titleSeen || !profileSeen) {
        error = "Preflight request fields are missing";
        return false;
    }
    // Legacy v1 protocol: a missing field is equivalent to a manual request.
    if (!automaticSeen) request.automaticLaunch = false;
    return validRequest(request, error);
}

bool loadPreflightRequest(
    const std::string& path,
    PreflightRequest& request,
    std::string& error) {
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError, 4 * 1024 * 1024)) {
        error = readError == ENOENT ? std::string() : "Preflight request is not readable";
        return false;
    }
    return parsePreflightRequest(text, request, error);
}

bool writePreflightRequestAtomic(
    const std::string& path,
    const PreflightRequest& request,
    int& systemError) {
    std::string error;
    if (!validRequest(request, error)) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, serializePreflightRequest(request), systemError);
}

bool completePreflightRequest(
    const std::string& path,
    const std::uint64_t expectedSequence,
    int& systemError) {
    PreflightRequest current;
    std::string error;
    if (!loadPreflightRequest(path, current, error)) {
        if (error.empty()) return true;
        systemError = EIO;
        return false;
    }
    if (current.sequence != expectedSequence) {
        systemError = EBUSY;
        return false;
    }
    return removeTextFileRecoverable(path, systemError);
}

std::string serializePreflightStatus(const PreflightStatus& status) {
    std::string text = "version=" + std::to_string(status.version) + "\n"
        + "worker_build_version=" + escapeValue(status.workerBuildVersion) + "\n"
        + "sequence=" + std::to_string(status.sequence) + "\n"
        + "title_id=" + status.titleId + "\n"
        + "profile_uid=" + status.profileUid + "\n"
        + "state=" + escapeValue(status.state) + "\n"
        + "outcome=" + escapeValue(status.outcome) + "\n"
        + "message=" + escapeValue(status.message) + "\n"
        + "valid_heads=" + std::to_string(status.validHeads) + "\n"
        + "invalid_heads=" + std::to_string(status.invalidHeads) + "\n"
        + "selected_device_id=" + escapeValue(status.selectedDeviceId) + "\n"
        + "selected_profile_name=" + escapeValue(status.selectedProfileName) + "\n"
        + "selected_revision_id=" + escapeValue(status.selectedRevisionId) + "\n"
        + "selected_archive_path=" + escapeValue(status.selectedArchivePath) + "\n"
        + "selected_game_version=" + escapeValue(status.selectedGameVersion) + "\n"
        + "selected_created_utc=" + escapeValue(status.selectedCreatedUtc) + "\n"
        + "selected_payload_sha256="
            + escapeValue(status.selectedPayloadSha256) + "\n"
        + "local_revision_id=" + escapeValue(status.localRevisionId) + "\n"
        + "local_payload_sha256=" + escapeValue(status.localPayloadSha256) + "\n"
        + "candidate_count=" + std::to_string(status.candidates.size()) + "\n";
    for (std::size_t index = 0; index < status.candidates.size(); ++index) {
        const PreflightCandidate& candidate = status.candidates[index];
        const std::string prefix = "candidate_" + std::to_string(index) + "_";
        text += prefix + "device_id=" + escapeValue(candidate.deviceId) + "\n";
        text += prefix + "profile_name=" + escapeValue(candidate.profileName) + "\n";
        text += prefix + "revision_id=" + candidate.revisionId + "\n";
        text += prefix + "payload_sha256=" + candidate.payloadSha256 + "\n";
        text += prefix + "archive_path=" + escapeValue(candidate.archivePath) + "\n";
        text += prefix + "game_version=" + escapeValue(candidate.gameVersion) + "\n";
        text += prefix + "created_utc=" + escapeValue(candidate.createdUtc) + "\n";
    }
    return text;
}

bool parsePreflightStatus(
    const std::string& text,
    PreflightStatus& status,
    std::string& error) {
    status = PreflightStatus{};
    bool versionSeen = false;
    bool buildSeen = false;
    bool sequenceSeen = false;
    bool titleSeen = false;
    bool profileSeen = false;
    bool stateSeen = false;
    bool outcomeSeen = false;
    bool messageSeen = false;
    bool validSeen = false;
    bool invalidSeen = false;
    bool candidateCountSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            std::string value;
            if (!unescapeValue(line.substr(separator + 1), value)) {
                error = "Invalid escape sequence in preflight status";
                return false;
            }
            std::uint64_t number = 0;
            if (key == "version" && parseUnsigned(value, number)) {
                status.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "worker_build_version") {
                status.workerBuildVersion = value;
                buildSeen = true;
            } else if (key == "sequence" && parseUnsigned(value, number)) {
                status.sequence = number;
                sequenceSeen = true;
            } else if (key == "title_id") {
                status.titleId = value;
                titleSeen = true;
            } else if (key == "profile_uid") {
                status.profileUid = value;
                profileSeen = true;
            } else if (key == "state") {
                status.state = value;
                stateSeen = true;
            } else if (key == "outcome") {
                status.outcome = value;
                outcomeSeen = true;
            } else if (key == "message") {
                status.message = value;
                messageSeen = true;
            } else if (key == "valid_heads" && parseUnsigned(value, number)) {
                status.validHeads = static_cast<std::size_t>(number);
                validSeen = true;
            } else if (key == "invalid_heads" && parseUnsigned(value, number)) {
                status.invalidHeads = static_cast<std::size_t>(number);
                invalidSeen = true;
            } else if (key == "candidate_count" && parseUnsigned(value, number)
                && number <= 128) {
                status.candidates.resize(static_cast<std::size_t>(number));
                candidateCountSeen = true;
            } else if (key == "selected_device_id") status.selectedDeviceId = value;
            else if (key == "selected_profile_name") status.selectedProfileName = value;
            else if (key == "selected_revision_id") status.selectedRevisionId = value;
            else if (key == "selected_archive_path") status.selectedArchivePath = value;
            else if (key == "selected_game_version") status.selectedGameVersion = value;
            else if (key == "selected_created_utc") status.selectedCreatedUtc = value;
            else if (key == "selected_payload_sha256") {
                status.selectedPayloadSha256 = value;
            }
            else if (key == "local_revision_id") status.localRevisionId = value;
            else if (key == "local_payload_sha256") {
                status.localPayloadSha256 = value;
            }
            else if (candidateCountSeen && key.rfind("candidate_", 0) == 0) {
                const std::size_t indexEnd = key.find('_', 10);
                std::uint64_t index = 0;
                if (indexEnd != std::string::npos
                    && parseUnsigned(key.substr(10, indexEnd - 10), index)
                    && index < status.candidates.size()) {
                    PreflightCandidate& candidate =
                        status.candidates[static_cast<std::size_t>(index)];
                    const std::string field = key.substr(indexEnd + 1);
                    if (field == "device_id") candidate.deviceId = value;
                    else if (field == "profile_name") candidate.profileName = value;
                    else if (field == "revision_id") candidate.revisionId = value;
                    else if (field == "payload_sha256") candidate.payloadSha256 = value;
                    else if (field == "archive_path") candidate.archivePath = value;
                    else if (field == "game_version") candidate.gameVersion = value;
                    else if (field == "created_utc") candidate.createdUtc = value;
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !buildSeen || !sequenceSeen || !titleSeen || !profileSeen
        || !stateSeen || !outcomeSeen || !messageSeen || !validSeen || !invalidSeen) {
        error = "Preflight status fields are missing";
        return false;
    }
    return validStatus(status, error);
}

bool loadPreflightStatus(
    const std::string& path,
    PreflightStatus& status,
    std::string& error) {
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError, 4 * 1024 * 1024)) {
        error = "Preflight status is unavailable";
        return false;
    }
    return parsePreflightStatus(text, status, error);
}

bool writePreflightStatusAtomic(
    const std::string& path,
    const PreflightStatus& status,
    int& systemError) {
    std::string error;
    if (!validStatus(status, error)) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, serializePreflightStatus(status), systemError);
}

} // namespace nxsync
