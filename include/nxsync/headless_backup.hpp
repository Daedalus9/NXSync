#pragma once

#include "nxsync/backup_manager.hpp"
#include "nxsync/device_identity.hpp"
#include "nxsync/local_resolution.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nxsync {

struct HeadlessBackupResult {
    bool success{false};
    bool transientUnavailable{false};
    Result mountResult{0};
    std::size_t matchedSaves{0};
    std::size_t createdArchives{0};
    std::size_t unchangedSaves{0};
    std::size_t emptySaves{0};
    std::size_t queuedCloudUploads{0};
    std::size_t localPruned{0};
    std::size_t retentionFailed{0};
    bool localResolutionApplied{false};
    std::string lastArchivePath;
    std::string message;
};

DeviceIdentity loadHeadlessDeviceIdentity(
    const std::string& appConfigPath,
    const std::string& fallbackIdPath);

HeadlessBackupResult createHeadlessBackupsForTitle(
    const DeviceIdentity& identity,
    std::uint64_t applicationId,
    const std::string& cloudQueueRoot,
    const std::string& remoteRoot,
    const std::string& storageEnvironment,
    bool queueCloudUploads,
    bool (*applicationRunning)(void* context),
    void* applicationContext,
    BackupProgressCallback progressCallback = nullptr,
    void* progressContext = nullptr,
    const PendingLocalResolution* localResolution = nullptr,
    std::size_t retentionCount = 0);

} // namespace nxsync
