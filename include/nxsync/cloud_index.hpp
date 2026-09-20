#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

constexpr const char* CloudIndexSchema = "nxsync-cloud-head";
constexpr unsigned CloudIndexVersion = 1;

struct CloudIndexEntry {
    std::string titleId;
    std::string titleName;
    std::string gameVersion;
    std::string deviceId;
    std::string profileName;
    std::string profileUid;
    std::string revisionId;
    std::string parentRevisionId;
    std::vector<std::string> parentRevisionIds;
    std::string payloadSha256;
    std::string archiveSha256;
    std::string archiveRemotePath;
    std::string createdUtc;
    std::uint64_t archiveSize{0};
    std::uint64_t uncompressedBytes{0};
    std::size_t fileCount{0};
};

std::string makeCloudIndexEntryRemotePath(
    const std::string& remoteRoot,
    const std::string& titleId,
    const std::string& deviceId,
    const std::string& profileUid);

std::string makeCloudRevisionRemotePath(
    const std::string& remoteRoot,
    const std::string& titleId,
    const std::string& revisionId);

bool validateCloudIndexEntry(const CloudIndexEntry& entry, std::string& error);
std::string serializeCloudIndexEntry(const CloudIndexEntry& entry);
bool parseCloudIndexEntry(
    const std::string& json,
    CloudIndexEntry& entry,
    std::string& error);

} // namespace nxsync
