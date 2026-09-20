#pragma once

#include "nxsync/backup_manager.hpp"
#include "nxsync/device_identity.hpp"
#include "nxsync/save_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

struct RestoreManifest {
    bool valid{false};
    unsigned formatVersion{0};
    std::string revisionId;
    std::string parentRevisionId;
    std::vector<std::string> parentRevisionIds;
    std::string payloadSha256;
    std::uint64_t titleId{0};
    std::string titleName;
    std::string gameVersion;
    std::string sourceDevice;
    std::string sourceProfile;
    std::string sourceProfileUid;
    std::string createdUtc;
    std::uint64_t ownerId{0};
    std::int64_t dataSize{0};
    std::int64_t journalSize{0};
    std::uint32_t saveFlags{0};
    std::uint16_t saveDataIndex{0};
    std::size_t fileCount{0};
    std::uint64_t uncompressedBytes{0};
};

struct RestoreInspection {
    bool success{false};
    int zipError{0};
    std::string message;
    RestoreManifest manifest;
    std::size_t validatedFiles{0};
    std::uint64_t validatedBytes{0};
};

struct RestoreResult {
    bool success{false};
    Result systemResult{0};
    int systemError{0};
    std::string message;
    std::string diagnostic;
    std::string safetyBackupPath;
    bool createdContainer{false};
    bool destinationModified{false};
    bool recoveryAttempted{false};
    bool recoverySucceeded{false};
    bool safetyBackupRestored{false};
    bool lineageRecorded{false};
    int lineageSystemError{0};
    std::size_t restoredFiles{0};
    std::uint64_t restoredBytes{0};
};

RestoreInspection inspectRestoreArchive(const std::string& archivePath);

RestoreResult restoreArchiveToProfile(
    const std::string& archivePath,
    const RestoreInspection& inspection,
    const DeviceIdentity& destinationIdentity,
    const UserSaves& destinationUser,
    BackupProgressCallback progressCallback = nullptr,
    void* progressContext = nullptr);

} // namespace nxsync
