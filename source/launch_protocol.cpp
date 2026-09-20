#include "nxsync/launch_protocol.hpp"
#include "nxsync/atomic_file.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <limits>

namespace nxsync {
namespace {

bool isHex(const std::string& value, const std::size_t length) {
    return value.size() == length
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

bool parseUnsigned(const std::string& value, std::uint64_t& output) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

std::string escapeValue(const std::string& value) {
    std::string result;
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

bool writeAtomic(const std::string& path, const std::string& text, int& systemError) {
    return writeTextFileAtomic(path, text, systemError);
}

template <typename Consumer>
bool parseLines(const std::string& text, Consumer&& consume, std::string& error) {
    if (text.empty() || text.back() != '\n' || text.find('\0') != std::string::npos) {
        error = "Invalid launch request"; return false;
    }
    std::set<std::string> keys;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || !keys.insert(line.substr(0, separator)).second) {
            error = "Invalid or duplicate launch protocol field"; return false;
        }
        if (separator != std::string::npos) {
            std::string value;
            if (!unescapeValue(line.substr(separator + 1), value)) {
                error = "Invalid escape sequence in the launch protocol";
                return false;
            }
            consume(line.substr(0, separator), value);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool readText(const std::string& path, std::string& text, std::string& error) {
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError)) {
        error = readError == ENOENT ? std::string() : "Launch protocol file is not readable";
        return false;
    }
    return true;
}

bool validActionName(const std::string& value) {
    return value == "allow" || value == "abort";
}

} // namespace

bool writeLaunchPhaseAtomic(const std::string& path, const std::uint64_t sequence,
                            const std::string& phase, int& systemError) {
    if (sequence == 0 || (phase != "choice" && phase != "download")) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, "version=" + std::to_string(LaunchProtocolVersion)
        + "\nsequence=" + std::to_string(sequence) + "\nphase=" + phase + "\n", systemError);
}

std::string serializeLaunchRequest(const LaunchRequest& request) {
    return "version=" + std::to_string(request.version) + "\n"
        + "sequence=" + std::to_string(request.sequence) + "\n"
        + "process_id=" + std::to_string(request.processId) + "\n"
        + "title_id=" + request.titleId + "\n";
}

bool parseLaunchRequest(
    const std::string& text,
    LaunchRequest& request,
    std::string& error) {
    request = LaunchRequest{};
    bool versionSeen = false, sequenceSeen = false, processSeen = false, titleSeen = false;
    if (!parseLines(text, [&](const std::string& key, const std::string& value) {
        std::uint64_t number = 0;
        if (key == "version" && parseUnsigned(value, number) && number <= std::numeric_limits<unsigned>::max()) {
            request.version = static_cast<unsigned>(number); versionSeen = true;
        } else if (key == "sequence" && parseUnsigned(value, number)) {
            request.sequence = number; sequenceSeen = true;
        } else if (key == "process_id" && parseUnsigned(value, number)) {
            request.processId = number; processSeen = true;
        } else if (key == "title_id") {
            request.titleId = value; titleSeen = true;
        }
    }, error)) return false;
    if (!versionSeen || !sequenceSeen || !processSeen || !titleSeen
        || request.version != LaunchProtocolVersion || request.sequence == 0
        || request.processId == 0 || !isHex(request.titleId, 16)) {
        error = "Invalid launch request";
        return false;
    }
    std::transform(
        request.titleId.begin(),
        request.titleId.end(),
        request.titleId.begin(),
        [](const unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
    error.clear();
    return true;
}

bool loadLaunchRequest(
    const std::string& path,
    LaunchRequest& request,
    std::string& error) {
    std::string text;
    return readText(path, text, error) && parseLaunchRequest(text, request, error);
}

std::string serializeLaunchDecision(const LaunchDecision& decision) {
    return "version=" + std::to_string(decision.version) + "\n"
        + "sequence=" + std::to_string(decision.sequence) + "\n"
        + "action=" + decision.action + "\n"
        + "message=" + escapeValue(decision.message) + "\n";
}

bool parseLaunchDecision(
    const std::string& text,
    LaunchDecision& decision,
    std::string& error) {
    decision = LaunchDecision{};
    bool versionSeen = false, sequenceSeen = false, actionSeen = false, messageSeen = false;
    if (!parseLines(text, [&](const std::string& key, const std::string& value) {
        std::uint64_t number = 0;
        if (key == "version" && parseUnsigned(value, number) && number <= std::numeric_limits<unsigned>::max()) {
            decision.version = static_cast<unsigned>(number); versionSeen = true;
        } else if (key == "sequence" && parseUnsigned(value, number)) {
            decision.sequence = number; sequenceSeen = true;
        } else if (key == "action") {
            decision.action = value; actionSeen = true;
        } else if (key == "message") {
            decision.message = value; messageSeen = true;
        }
    }, error)) return false;
    if (!versionSeen || !sequenceSeen || !actionSeen || !messageSeen
        || decision.version != LaunchProtocolVersion || decision.sequence == 0
        || !validActionName(decision.action)) {
        error = "Invalid launch decision";
        return false;
    }
    error.clear();
    return true;
}

bool loadLaunchDecision(
    const std::string& path,
    LaunchDecision& decision,
    std::string& error) {
    std::string text;
    return readText(path, text, error) && parseLaunchDecision(text, decision, error);
}

bool writeLaunchDecisionAtomic(
    const std::string& path,
    const LaunchDecision& decision,
    int& systemError) {
    std::string error;
    LaunchDecision parsed;
    if (!parseLaunchDecision(serializeLaunchDecision(decision), parsed, error)) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, serializeLaunchDecision(decision), systemError);
}

std::string serializeLaunchAction(const LaunchAction& action) {
    return "version=" + std::to_string(action.version) + "\n"
        + "sequence=" + std::to_string(action.sequence) + "\n"
        + "action=" + action.action + "\n"
        + "selected_revision_id=" + action.selectedRevisionId + "\n";
}

bool parseLaunchAction(
    const std::string& text,
    LaunchAction& action,
    std::string& error) {
    action = LaunchAction{};
    bool versionSeen = false, sequenceSeen = false, actionSeen = false, revisionSeen = false;
    if (!parseLines(text, [&](const std::string& key, const std::string& value) {
        std::uint64_t number = 0;
        if (key == "version" && parseUnsigned(value, number) && number <= std::numeric_limits<unsigned>::max()) {
            action.version = static_cast<unsigned>(number); versionSeen = true;
        } else if (key == "sequence" && parseUnsigned(value, number)) {
            action.sequence = number; sequenceSeen = true;
        } else if (key == "action") {
            action.action = value; actionSeen = true;
        } else if (key == "selected_revision_id") {
            action.selectedRevisionId = value; revisionSeen = true;
        }
    }, error)) return false;
    if (!versionSeen || !sequenceSeen || !actionSeen || !revisionSeen
        || action.version != LaunchProtocolVersion || action.sequence == 0
        || (action.action != "use-local" && action.action != "restore-cloud")
        || (!action.selectedRevisionId.empty()
            && !isHex(action.selectedRevisionId, 64))) {
        error = "Invalid launch action";
        return false;
    }
    error.clear();
    return true;
}

bool loadLaunchAction(
    const std::string& path,
    LaunchAction& action,
    std::string& error) {
    std::string text;
    return readText(path, text, error) && parseLaunchAction(text, action, error);
}

bool writeLaunchActionAtomic(
    const std::string& path,
    const LaunchAction& action,
    int& systemError) {
    std::string error;
    LaunchAction parsed;
    if (!parseLaunchAction(serializeLaunchAction(action), parsed, error)) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, serializeLaunchAction(action), systemError);
}

bool completeLaunchAction(
    const std::string& path,
    const std::uint64_t expectedSequence,
    int& systemError) {
    LaunchAction current;
    std::string error;
    if (!loadLaunchAction(path, current, error)) {
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

bool requestLaunchOverlay(const std::string& path, int& systemError) {
    return writeAtomic(path, "nxsync-launch-gate\n", systemError);
}


std::string serializeLaunchRestoreLease(const LaunchRestoreLease& lease) {
    return serializeLaunchRequest(lease.request) + "profile_uid=" + lease.profileUid + "\n";
}
bool parseLaunchRestoreLease(const std::string& text, LaunchRestoreLease& lease, std::string& error) {
    lease = {};
    if (!parseLaunchRequest(text, lease.request, error)) return false;
    const auto start = text.find("\nprofile_uid=");
    if (start == std::string::npos) return false;
    lease.profileUid = text.substr(start + 13, 32);
    if (!isHex(lease.profileUid, 32) || serializeLaunchRestoreLease(lease) != text) {
        error = "Invalid restore lease"; return false;
    }
    return true;
}
bool loadLaunchRestoreLease(const std::string& path, LaunchRestoreLease& lease, std::string& error) {
    std::string text;
    return readText(path, text, error) && parseLaunchRestoreLease(text, lease, error);
}
bool writeLaunchRestoreLease(const std::string& path, const LaunchRestoreLease& lease, int& error) {
    const auto text = serializeLaunchRestoreLease(lease);
    LaunchRestoreLease parsed;
    std::string validation;
    if (!parseLaunchRestoreLease(text, parsed, validation)) { error = EINVAL; return false; }
    return writeTextFileAtomic(path, text, error);
}
bool launchRestoreBlocksTitle(const std::string& titleId, const std::string& path) {
    std::string text;
    int systemError = 0;
    if (!readTextFileRecoverable(path, text, systemError)) return systemError != ENOENT;
    LaunchRestoreLease lease;
    std::string error;
    if (!parseLaunchRestoreLease(text, lease, error)) return true;
    std::string normalized = titleId;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return lease.request.titleId == normalized;
}

bool clearRecoveredLaunchGuard(const std::string& titleId, const std::string& profileUid, int& error,
                               const std::string& path) {
    error = 0;
    LaunchRestoreLease lease;
    std::string message;
    if (!loadLaunchRestoreLease(path, lease, message)) {
        std::string text;
        // A malformed/unreadable guard is never silently discarded.
        if (!readTextFileRecoverable(path, text, error) && error == ENOENT) {
            error = 0;
            return true;
        }
        error = EINVAL; return false;
    }
    if (lease.request.titleId != titleId || lease.profileUid != profileUid) { error = EBUSY; return false; }
    return removeTextFileRecoverable(path, error);
}

} // namespace nxsync
