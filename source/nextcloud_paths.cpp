#include "nxsync/nextcloud_paths.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace nxsync {
namespace {

bool isUnreserved(const unsigned char ch) {
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string percentEncode(const std::string& value) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char ch : value) {
        if (isUnreserved(ch)) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(Hex[ch >> 4U]);
            encoded.push_back(Hex[ch & 0x0FU]);
        }
    }
    return encoded;
}

std::string trimTrailingSlashes(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

} // namespace

bool downloadHashMatches(const std::string& remotePath,
                         const std::string& localSha256,
                         const std::string& serverSha256) {
    const auto normalizedHash = [](std::string hash) {
        std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return hash;
    };
    const auto local = normalizedHash(localSha256);
    if (local.size() != 64 || !std::all_of(local.begin(), local.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        })) return false;
    if (!serverSha256.empty() && normalizedHash(serverSha256) != local) return false;
    const auto slash = remotePath.find_last_of('/');
    const auto filename = remotePath.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto underscore = filename.find_last_of('_');
    if (underscore == std::string::npos || filename.size() < underscore + 5
        || filename.compare(filename.size() - 4, 4, ".zip") != 0) return false;
    const auto prefix = normalizedHash(filename.substr(underscore + 1, filename.size() - underscore - 5));
    return prefix.size() >= 12 && prefix.size() <= 64
        && local.compare(0, prefix.size(), prefix) == 0;
}

bool isValidNextcloudUrl(const std::string& configuredUrl) {
    if (configuredUrl.size() < 9
        || configuredUrl.find('?') != std::string::npos
        || configuredUrl.find('#') != std::string::npos) {
        return false;
    }

    std::string scheme = configuredUrl.substr(0, 8);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (scheme != "https://") {
        return false;
    }

    const std::size_t authorityEnd = configuredUrl.find('/', 8);
    const std::size_t authorityLength = authorityEnd == std::string::npos
        ? configuredUrl.size() - 8
        : authorityEnd - 8;
    if (authorityLength == 0) {
        return false;
    }

    for (const unsigned char ch : configuredUrl) {
        if (std::iscntrl(ch) || std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

std::string makeNextcloudDavBaseUrl(
    const std::string& configuredUrl,
    const std::string& username) {
    std::string base = trimTrailingSlashes(configuredUrl);
    if (base.find("/remote.php/dav/files/") != std::string::npos) {
        return base;
    }
    return base + "/remote.php/dav/files/" + percentEncode(username);
}

std::string makeNextcloudResourceUrl(
    const std::string& davBaseUrl,
    const std::string& remotePath) {
    std::string url = trimTrailingSlashes(davBaseUrl);
    std::string segment;
    for (std::size_t index = 0; index <= remotePath.size(); ++index) {
        const bool atEnd = index == remotePath.size();
        if (!atEnd && remotePath[index] != '/') {
            segment.push_back(remotePath[index]);
            continue;
        }
        if (!segment.empty()) {
            url.push_back('/');
            url += percentEncode(segment);
            segment.clear();
        }
    }
    return url;
}

bool isMutableCloudIndexPath(const std::string& remotePath) {
    return remotePath.find("/_index/titles/") != std::string::npos;
}

std::string makeFreshNextcloudResourceUrl(
    const std::string& davBaseUrl,
    const std::string& remotePath,
    const std::uint64_t cacheBuster) {
    std::string url = makeNextcloudResourceUrl(davBaseUrl, remotePath);
    if (isMutableCloudIndexPath(remotePath)) {
        url += "?nxsync_cb=" + std::to_string(cacheBuster);
    }
    return url;
}

} // namespace nxsync
