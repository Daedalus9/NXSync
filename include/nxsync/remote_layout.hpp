#pragma once

#include <string>

namespace nxsync {

std::string sanitizePathSegment(const std::string& value, bool allowDot = false);
// Stable identity; display names never identify an archive/retention namespace.
std::string makeProfileBackupFolder(const std::string& profileUid);
bool isProfileBackupDirectory(const std::string& directory, const std::string& profileUid);

std::string normalizeRemoteRoot(const std::string& remoteRoot);
std::string makeDeviceRemoteRoot(
    const std::string& remoteRoot,
    const std::string& deviceFolder);

std::string makeBackupRemotePath(
    const std::string& remoteRoot,
    const std::string& deviceFolder,
    const std::string& profileUid,
    const std::string& titleId,
    const std::string& fileName);

} // namespace nxsync
