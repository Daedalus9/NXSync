#include "nxsync/headless_backup.hpp"

#include "nxsync/backup_manager.hpp"
#include "nxsync/backup_state.hpp"
#include "nxsync/cloud_queue.hpp"
#include "nxsync/display_text.hpp"
#include "nxsync/launch_protocol.hpp"
#include "nxsync/remote_layout.hpp"
#include "nxsync/retention.hpp"
#include "nxsync/save_catalog.hpp"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace nxsync {
namespace {

constexpr std::size_t ReaderBatchSize = 16;

bool uidEquals(const AccountUid& left, const AccountUid& right) {
    return left.uid[0] == right.uid[0] && left.uid[1] == right.uid[1];
}

std::string trim(std::string value) {
    const auto visible = [](const unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), visible));
    value.erase(std::find_if(value.rbegin(), value.rend(), visible).base(), value.end());
    return value;
}

std::string readDeviceOverride(const std::string& path) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        if (trim(line.substr(0, separator)) == "device_id_override") {
            return trim(line.substr(separator + 1));
        }
    }
    return {};
}

std::string uidLabel(const AccountUid& uid) {
    char buffer[40]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "UID %08llX-%08llX",
        static_cast<unsigned long long>(uid.uid[1] & 0xFFFFFFFFULL),
        static_cast<unsigned long long>(uid.uid[0] & 0xFFFFFFFFULL));
    return buffer;
}

std::string formatUid(const AccountUid& uid) {
    char buffer[33]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llX%016llX",
        static_cast<unsigned long long>(uid.uid[1]),
        static_cast<unsigned long long>(uid.uid[0]));
    return buffer;
}

std::string fileNameFromPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string parentPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

bool queueCloudUpload(
    const std::string& queueRoot,
    const std::string& remoteRoot,
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    const LocalBackupState& state,
    const std::string& storageEnvironment,
    int& systemError) {
    PendingCloudOperation operation;
    operation.storageEnvironment = storageEnvironment;
    operation.revisionId = state.revisionId;
    operation.titleId = formatTitleId(save.applicationId);
    operation.saveDataId = formatTitleId(save.saveDataId);
    operation.profileUid = formatUid(user.uid);
    operation.archivePath = state.archivePath;
    operation.remotePath = makeBackupRemotePath(
        remoteRoot,
        identity.folderName,
        operation.profileUid,
        operation.titleId,
        fileNameFromPath(state.archivePath));
    operation.archiveSha256 = state.sha256;
    return enqueueCloudOperation(queueRoot, operation, systemError);
}

std::vector<UserSaves> loadRegisteredUsers(Result& result) {
    std::vector<UserSaves> users;
    result = accountInitialize(AccountServiceType_Administrator);
    if (R_FAILED(result)) return users;
    std::array<AccountUid, ACC_USER_LIST_SIZE> uids{};
    s32 total = 0;
    result = accountListAllUsers(uids.data(), static_cast<s32>(uids.size()), &total);
    if (R_SUCCEEDED(result)) {
        total = std::max<s32>(0, std::min<s32>(total, uids.size()));
        for (s32 index = 0; index < total; ++index) {
            UserSaves user;
            user.uid = uids[static_cast<std::size_t>(index)];
            user.nickname = uidLabel(user.uid);
            user.registeredProfile = true;
            AccountProfile profile{};
            if (R_SUCCEEDED(accountGetProfile(&profile, user.uid))) {
                AccountProfileBase base{};
                if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base))) {
                    const std::string nickname = sanitizeDisplayUtf8(
                        base.nickname,
                        sizeof(base.nickname)).value;
                    if (!nickname.empty()) user.nickname = nickname;
                }
                accountProfileClose(&profile);
            }
            users.push_back(std::move(user));
        }
    }
    accountExit();
    return users;
}

UserSaves* findUser(std::vector<UserSaves>& users, const AccountUid& uid) {
    const auto found = std::find_if(users.begin(), users.end(), [&](const auto& user) {
        return uidEquals(user.uid, uid);
    });
    return found == users.end() ? nullptr : &*found;
}

bool loadMatchingSaves(
    std::vector<UserSaves>& users,
    const std::uint64_t applicationId,
    Result& result) {
    FsSaveDataInfoReader reader{};
    result = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_All);
    if (R_FAILED(result)) return false;
    std::array<FsSaveDataInfo, ReaderBatchSize> batch{};
    while (true) {
        s64 entriesRead = 0;
        result = fsSaveDataInfoReaderRead(
            &reader, batch.data(), batch.size(), &entriesRead);
        if (R_FAILED(result) || entriesRead <= 0) break;
        const std::size_t bounded = std::min<std::size_t>(entriesRead, batch.size());
        for (std::size_t index = 0; index < bounded; ++index) {
            const FsSaveDataInfo& info = batch[index];
            if (info.application_id != applicationId
                || info.save_data_type != FsSaveDataType_Account
                || info.save_data_rank != FsSaveDataRank_Primary
                || !accountUidIsValid(&info.uid)) continue;
            UserSaves* user = findUser(users, info.uid);
            if (user == nullptr) continue;
            SaveEntry save;
            save.applicationId = info.application_id;
            save.saveDataId = info.save_data_id;
            save.rawSize = info.size;
            save.saveDataSpaceId =
                static_cast<FsSaveDataSpaceId>(info.save_data_space_id);
            save.saveDataIndex = info.save_data_index;
            save.saveDataRank = info.save_data_rank;
            save.titleName = "Title " + formatTitleId(applicationId);
            FsSaveDataExtraData extra{};
            if (R_SUCCEEDED(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
                    &extra,
                    sizeof(extra),
                    save.saveDataSpaceId,
                    save.saveDataId))) {
                save.commitId = extra.commit_id;
                save.saveTimestamp = extra.timestamp;
                save.ownerId = extra.owner_id;
                save.dataSize = extra.data_size;
                save.journalSize = extra.journal_size;
                save.flags = extra.flags;
                save.extraDataAvailable = true;
            }
            user->saves.push_back(std::move(save));
        }
    }
    fsSaveDataInfoReaderClose(&reader);
    return R_SUCCEEDED(result);
}

} // namespace

DeviceIdentity loadHeadlessDeviceIdentity(
    const std::string& appConfigPath,
    const std::string& fallbackIdPath) {
    return detectDeviceIdentity(readDeviceOverride(appConfigPath), fallbackIdPath);
}

HeadlessBackupResult createHeadlessBackupsForTitle(
    const DeviceIdentity& identity,
    const std::uint64_t applicationId,
    const std::string& cloudQueueRoot,
    const std::string& remoteRoot,
    const std::string& storageEnvironment,
    const bool queueCloudUploads,
    bool (*applicationRunning)(void* context),
    void* applicationContext,
    BackupProgressCallback progressCallback,
    void* progressContext,
    const PendingLocalResolution* localResolution,
    const std::size_t retentionCount) {
    HeadlessBackupResult result;
    if (launchRestoreBlocksTitle(formatTitleId(applicationId))) {
        result.message = "Save recovery required; backups and retention are paused";
        result.transientUnavailable = true;
        return result;
    }
    Result serviceResult = 0;
    std::vector<UserSaves> users = loadRegisteredUsers(serviceResult);
    if (R_FAILED(serviceResult)) {
        result.message = "Profile service unavailable: "
            + formatResult(serviceResult);
        return result;
    }
    if (!loadMatchingSaves(users, applicationId, serviceResult)) {
        result.message = "Save catalog unavailable: "
            + formatResult(serviceResult);
        return result;
    }
    for (UserSaves& user : users) {
        const bool resolvesThisProfile = localResolution != nullptr
            && localResolution->titleId == formatTitleId(applicationId)
            && localResolution->profileUid == formatUid(user.uid);
        if (resolvesThisProfile) {
            int anchorError = 0;
            if (!writeRestoreLineageAnchor(
                    identity,
                    user,
                    applicationId,
                    localResolution->parentRevisionId,
                    localResolution->parentPayloadSha256,
                    localResolution->parentRevisionIds,
                    anchorError)) {
                result.message =
                    "Unable to prepare conflict resolution (errno "
                    + std::to_string(anchorError) + ")";
                return result;
            }
        }
        for (SaveEntry& save : user.saves) {
            ++result.matchedSaves;
            LocalBackupState state = findCurrentLocalBackup(identity, user, save);
            const RestoreLineageAnchor lineageAnchor = loadRestoreLineageAnchor(
                identity, user, save.applicationId);
            const bool lineageBackupRequired =
                validateRestoreLineageAnchor(lineageAnchor);
            if (state.current && !lineageBackupRequired) {
                ++result.unchangedSaves;
            } else if (applicationRunning != nullptr
                && applicationRunning(applicationContext)) {
                result.transientUnavailable = true;
                result.message = "Backup postponed: an application was launched";
                return result;
            } else {
                const BackupResult backup = createLocalBackup(
                    identity,
                    user,
                    save,
                    progressCallback,
                    progressContext);
                if (backup.success) {
                    ++result.createdArchives;
                    result.lastArchivePath = backup.archivePath;
                    if (resolvesThisProfile
                        && backup.parentRevisionIds
                            == localResolution->parentRevisionIds
                        && backup.lineageAnchorConsumed) {
                        result.localResolutionApplied = true;
                    }
                    state = findCurrentLocalBackup(identity, user, save);
                } else if (backup.emptySave) {
                    ++result.emptySaves;
                    continue;
                } else {
                    result.mountResult = backup.mountResult;
                    result.transientUnavailable = R_FAILED(backup.mountResult);
                    result.message = backup.message.empty()
                        ? "Backup creation failed"
                        : backup.message;
                    return result;
                }
            }
            if (retentionCount > 0 && state.current && !state.archivePath.empty()
                && isProfileBackupDirectory(parentPath(state.archivePath), formatUid(user.uid))) {
                const RetentionResult retention = pruneLocalBackups(
                    parentPath(state.archivePath),
                    fileNameFromPath(state.archivePath),
                    retentionCount);
                result.localPruned += retention.removed;
                result.retentionFailed += retention.failed;
                if (retention.failed > 0 && result.message.empty()) {
                    result.message = retention.firstError;
                }
            }
            if (queueCloudUploads && state.current && !state.remoteUploaded) {
                int queueError = 0;
                if (!queueCloudUpload(
                        cloudQueueRoot,
                        remoteRoot,
                        identity,
                        user,
                        save,
                        state,
                        storageEnvironment,
                        queueError)) {
                    result.message = "Backup created, but upload was not queued (errno "
                        + std::to_string(queueError) + ")";
                    return result;
                }
                ++result.queuedCloudUploads;
            }
        }
    }
    if (result.matchedSaves == 0) {
        result.message = "No Account/User saves for registered profiles";
        return result;
    }
    result.success = true;
    if (result.createdArchives > 0) {
        result.message = "Local backup verified";
    } else if (result.unchangedSaves > 0) {
        result.message = "Save unchanged";
    } else {
        result.message = "No save data for registered profiles";
    }
    if (result.retentionFailed > 0) {
        result.message += "; local retention was not completed";
    } else if (result.localPruned > 0) {
        result.message += "; removed "
            + std::to_string(result.localPruned) + " ZIP locali";
    }
    return result;
}

} // namespace nxsync
