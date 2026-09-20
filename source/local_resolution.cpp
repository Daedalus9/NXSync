#include "nxsync/local_resolution.hpp"
#include "nxsync/revision_parents.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace nxsync {
namespace {

bool isHex(const std::string& value, const std::size_t length) {
    return value.size() == length
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
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

std::string joinParents(const std::vector<std::string>& parents) {
    std::string result;
    for (const std::string& parent : parents) {
        if (!result.empty()) result.push_back(',');
        result += parent;
    }
    return result;
}

std::vector<std::string> splitParents(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(',', start);
        const std::string parent = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!parent.empty()) result.push_back(parent);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

bool writeAtomic(
    const std::string& path,
    const std::string& text,
    int& systemError) {
    const std::string temporary = path + ".new";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            systemError = errno;
            return false;
        }
        output << text;
        output.flush();
        if (!output) {
            systemError = EIO;
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        systemError = errno;
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    systemError = errno;
    std::remove(temporary.c_str());
    return false;
}

} // namespace

bool validatePendingLocalResolution(
    const PendingLocalResolution& resolution,
    std::string& error) {
    if ((resolution.version < 1 || resolution.version > LocalResolutionVersion)
        || resolution.sequence == 0
        || (resolution.storageEnvironment != "emummc"
            && resolution.storageEnvironment != "sysmmc")
        || !isHex(resolution.titleId, 16)
        || !isHex(resolution.profileUid, 32)
        || !isHex(resolution.parentRevisionId, 64)
        || !isHex(resolution.parentPayloadSha256, 64)
        || !validRevisionParents(
            std::string(),
            resolution.parentRevisionId,
            resolution.parentRevisionIds)) {
        error = "Invalid local resolution";
        return false;
    }
    error.clear();
    return true;
}

std::string serializePendingLocalResolution(
    const PendingLocalResolution& resolution) {
    const std::vector<std::string> parents = normalizedRevisionParents(
        resolution.parentRevisionId, resolution.parentRevisionIds);
    return "version=" + std::to_string(resolution.version) + "\n"
        + "sequence=" + std::to_string(resolution.sequence) + "\n"
        + "storage_environment=" + resolution.storageEnvironment + "\n"
        + "title_id=" + resolution.titleId + "\n"
        + "profile_uid=" + resolution.profileUid + "\n"
        + "parent_revision_id="
            + (parents.empty() ? std::string() : parents.front()) + "\n"
        + "parent_revision_ids=" + joinParents(parents) + "\n"
        + "parent_payload_sha256=" + resolution.parentPayloadSha256 + "\n";
}

bool parsePendingLocalResolution(
    const std::string& text,
    PendingLocalResolution& resolution,
    std::string& error) {
    resolution = PendingLocalResolution{};
    bool versionSeen = false;
    bool sequenceSeen = false;
    bool environmentSeen = false;
    bool titleSeen = false;
    bool profileSeen = false;
    bool revisionSeen = false;
    bool payloadSeen = false;
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
                resolution.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "sequence" && parseUnsigned(value, number)) {
                resolution.sequence = number;
                sequenceSeen = true;
            } else if (key == "storage_environment") {
                resolution.storageEnvironment = value;
                environmentSeen = true;
            } else if (key == "title_id") {
                resolution.titleId = value;
                titleSeen = true;
            } else if (key == "profile_uid") {
                resolution.profileUid = value;
                profileSeen = true;
            } else if (key == "parent_revision_id") {
                resolution.parentRevisionId = value;
                revisionSeen = true;
            } else if (key == "parent_revision_ids") {
                resolution.parentRevisionIds = splitParents(value);
            } else if (key == "parent_payload_sha256") {
                resolution.parentPayloadSha256 = value;
                payloadSeen = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !sequenceSeen || !environmentSeen || !titleSeen
        || !profileSeen || !revisionSeen || !payloadSeen) {
        error = "Local resolution fields are missing";
        return false;
    }
    resolution.parentRevisionIds = normalizedRevisionParents(
        resolution.parentRevisionId, resolution.parentRevisionIds);
    resolution.parentRevisionId = resolution.parentRevisionIds.empty()
        ? std::string()
        : resolution.parentRevisionIds.front();
    return validatePendingLocalResolution(resolution, error);
}

bool loadPendingLocalResolution(
    const std::string& path,
    PendingLocalResolution& resolution,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Local resolution is unavailable";
        return false;
    }
    const std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return parsePendingLocalResolution(text, resolution, error);
}

bool writePendingLocalResolutionAtomic(
    const std::string& path,
    const PendingLocalResolution& resolution,
    int& systemError) {
    std::string error;
    if (!validatePendingLocalResolution(resolution, error)) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, serializePendingLocalResolution(resolution), systemError);
}

bool completePendingLocalResolution(
    const std::string& path,
    const std::uint64_t expectedSequence,
    int& systemError) {
    PendingLocalResolution resolution;
    std::string error;
    if (!loadPendingLocalResolution(path, resolution, error)
        || resolution.sequence != expectedSequence) {
        systemError = EINVAL;
        return false;
    }
    if (std::remove(path.c_str()) == 0 || errno == ENOENT) return true;
    systemError = errno;
    return false;
}

} // namespace nxsync
