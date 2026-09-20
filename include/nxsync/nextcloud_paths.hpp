#pragma once

#include <cstdint>
#include <string>

namespace nxsync {

bool isValidNextcloudUrl(const std::string& configuredUrl);

// The filename identifies the requested archive. A server checksum is an
// additional check, never a substitute for that identity.
bool downloadHashMatches(const std::string& remotePath,
                         const std::string& localSha256,
                         const std::string& serverSha256);

std::string makeNextcloudDavBaseUrl(
    const std::string& configuredUrl,
    const std::string& username);

std::string makeNextcloudResourceUrl(
    const std::string& davBaseUrl,
    const std::string& remotePath);

// Title head documents are mutable pointers.  Unlike content-addressed
// revision records, they must never be served from an intermediary cache.
bool isMutableCloudIndexPath(const std::string& remotePath);

std::string makeFreshNextcloudResourceUrl(
    const std::string& davBaseUrl,
    const std::string& remotePath,
    std::uint64_t cacheBuster);

} // namespace nxsync
