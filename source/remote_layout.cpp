#include "nxsync/remote_layout.hpp"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace nxsync {
namespace {

std::string sanitizeRemoteSegment(const std::string& value, const bool allowDot) {
    std::string result;
    result.reserve(value.size());
    bool previousWasSeparator = false;

    for (const unsigned char ch : value) {
        const bool isAlphaNumeric =
            (ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9');
        const bool isAllowedPunctuation =
            ch == '-' || ch == '_' || (allowDot && ch == '.');

        if (isAlphaNumeric || isAllowedPunctuation) {
            result.push_back(static_cast<char>(ch));
            previousWasSeparator = false;
        } else if (!result.empty() && !previousWasSeparator) {
            result.push_back('-');
            previousWasSeparator = true;
        }
    }

    while (!result.empty() && (result.back() == '-' || result.back() == '.')) {
        result.pop_back();
    }
    while (!result.empty() && result.front() == '.') {
        result.erase(result.begin());
    }

    if (result == "." || result == "..") {
        return {};
    }
    return result;
}

std::string sanitizedOrFallback(
    const std::string& value,
    const std::string& fallback,
    const bool allowDot = false) {
    const std::string sanitized = sanitizeRemoteSegment(value, allowDot);
    return sanitized.empty() ? fallback : sanitized;
}

std::vector<std::string> splitRemotePath(const std::string& value) {
    std::vector<std::string> segments;
    std::string current;
    std::istringstream input(value);
    while (std::getline(input, current, '/')) {
        const std::string sanitized = sanitizeRemoteSegment(current, false);
        if (!sanitized.empty()) {
            segments.push_back(sanitized);
        }
    }
    return segments;
}

std::string joinRemotePath(const std::vector<std::string>& segments) {
    std::string result;
    for (const auto& segment : segments) {
        result.push_back('/');
        result += segment;
    }
    return result.empty() ? "/NXSync" : result;
}

} // namespace

std::string sanitizePathSegment(const std::string& value, const bool allowDot) {
    return sanitizeRemoteSegment(value, allowDot);
}

std::string normalizeRemoteRoot(const std::string& remoteRoot) {
    return joinRemotePath(splitRemotePath(remoteRoot));
}

std::string makeProfileBackupFolder(const std::string& profileUid) {
    if (profileUid.size() != 32 || !std::all_of(profileUid.begin(), profileUid.end(),
        [](unsigned char ch) { return std::isxdigit(ch) != 0; })) return {};
    std::string uid = profileUid;
    std::transform(uid.begin(), uid.end(), uid.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return "profile-" + uid;
}

bool isProfileBackupDirectory(const std::string& directory, const std::string& profileUid) {
    const std::string folder = makeProfileBackupFolder(profileUid);
    if (folder.empty()) return false;
    const auto titleSlash = directory.find_last_of('/');
    if (titleSlash == std::string::npos) return false;
    const auto profileSlash = directory.find_last_of('/', titleSlash - 1);
    if (profileSlash == std::string::npos) return false;
    return directory.substr(profileSlash + 1, titleSlash - profileSlash - 1) == folder;
}

std::string makeDeviceRemoteRoot(
    const std::string& remoteRoot,
    const std::string& deviceFolder) {
    return normalizeRemoteRoot(remoteRoot)
        + "/" + sanitizedOrFallback(deviceFolder, "NS-UNKNOWN");
}

std::string makeBackupRemotePath(
    const std::string& remoteRoot,
    const std::string& deviceFolder,
    const std::string& profileUid,
    const std::string& titleId,
    const std::string& fileName) {
    const auto profileFolder = makeProfileBackupFolder(profileUid);
    if (profileFolder.empty()) return {};
    return makeDeviceRemoteRoot(remoteRoot, deviceFolder)
        + "/" + profileFolder
        + "/" + sanitizedOrFallback(titleId, "UNKNOWN-TITLE")
        + "/" + sanitizedOrFallback(fileName, "backup.zip", true);
}

} // namespace nxsync
