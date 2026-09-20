#pragma once

#include "nxsync/device_identity.hpp"
#include "nxsync/revision_lineage.hpp"
#include "nxsync/save_catalog.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

struct LocalBackupState {
    bool indexed{false};
    bool legacy{false};
    bool emptySave{false};
    bool archivePresent{false};
    bool recordValid{false};
    bool saveChanged{false};
    bool current{false};
    std::string archivePath;
    std::string sha256;
    std::string revisionId;
    std::string parentRevisionId;
    std::vector<std::string> parentRevisionIds;
    std::string payloadSha256;
    std::string createdUtc;
    std::size_t fileCount{0};
    std::uint64_t uncompressedBytes{0};
    std::uint64_t archiveSize{0};
    bool remoteUploaded{false};
    std::string remotePath;
};

LocalBackupState findCurrentLocalBackup(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save);

bool writeLocalBackupState(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    const std::string& archivePath,
    const std::string& sha256,
    const std::string& revisionId,
    const std::string& parentRevisionId,
    const std::vector<std::string>& parentRevisionIds,
    const std::string& payloadSha256,
    const std::string& createdUtc,
    std::size_t fileCount,
    std::uint64_t uncompressedBytes,
    int& systemError);

bool writeLocalEmptySaveState(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    int& systemError);

bool markLocalBackupUploaded(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    const std::string& remotePath,
    int& systemError);

RestoreLineageAnchor loadRestoreLineageAnchor(
    const DeviceIdentity& identity,
    const UserSaves& user,
    std::uint64_t applicationId);

bool writeRestoreLineageAnchor(
    const DeviceIdentity& identity,
    const UserSaves& user,
    std::uint64_t applicationId,
    const std::string& revisionId,
    const std::string& payloadSha256,
    const std::vector<std::string>& parentRevisionIds,
    int& systemError);

bool clearRestoreLineageAnchor(
    const DeviceIdentity& identity,
    const UserSaves& user,
    std::uint64_t applicationId,
    int& systemError);

} // namespace nxsync
