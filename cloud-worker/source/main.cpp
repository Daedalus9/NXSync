#include "nxsync/app_config.hpp"
#include "nxsync/atomic_file.hpp"
#include "nxsync/backup_request_queue.hpp"
#include "nxsync/backup_state.hpp"
#include "nxsync/cloud_worker_policy.hpp"
#include "nxsync/cloud_worker_status.hpp"
#include "nxsync/cloud_index.hpp"
#include "nxsync/device_identity.hpp"
#include "nxsync/game_version.hpp"
#include "nxsync/headless_backup.hpp"
#include "nxsync/launch_protocol.hpp"
#include "nxsync/launch_gate_policy.hpp"
#include "nxsync/local_resolution.hpp"
#include "nxsync/nextcloud_client.hpp"
#include "nxsync/nextcloud_paths.hpp"
#include "nxsync/preflight.hpp"
#include "nxsync/preflight_protocol.hpp"
#include "nxsync/remote_layout.hpp"
#include "nxsync/retention.hpp"
#include "nxsync/revision_parents.hpp"
#include "nxsync/restore_manager.hpp"
#include "nxsync/save_catalog.hpp"
#include "nxsync/sync_engine.hpp"
#include "nxsync/storage_environment.hpp"
#include "nxsync/ultrahand_notification.hpp"

#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char* BuildVersion = "0.8.9-rc1";
constexpr const char* ConfigPath = "sdmc:/config/NXSync/config.ini";
constexpr const char* FallbackIdPath = "sdmc:/config/NXSync/device_id.txt";
constexpr const char* QueueRoot = "sdmc:/config/NXSync/queue";
constexpr const char* BackupRequestRoot = "sdmc:/config/NXSync/backup-requests";
constexpr const char* StatusPath = "sdmc:/config/NXSync/cloud-worker.status";
constexpr const char* LockPath = "sdmc:/config/NXSync/cloud-worker.lock";
constexpr const char* NotificationDirectory =
    "sdmc:/config/ultrahand/notifications";
constexpr const char* PreflightRequestPath =
    "sdmc:/config/NXSync/preflight.request";
constexpr const char* PreflightStatusPath =
    "sdmc:/config/NXSync/preflight.status";
constexpr const char* LaunchRequestPath =
    "sdmc:/config/NXSync/launch.request";
constexpr const char* LaunchWorkerResultPath =
    "sdmc:/config/NXSync/launch.worker-result";
constexpr const char* LaunchActionPath =
    "sdmc:/config/NXSync/launch.action";
constexpr const char* LocalResolutionPath =
    "sdmc:/config/NXSync/local-resolution.pending";
constexpr const char* LaunchRestoreDownloadPath =
    "sdmc:/switch/NXSync/imports/launch-restore.zip";
// Socket transfer memory, the save catalog, libcurl, restore validation and the
// SSL backend coexist during startup, so keep enough allocator headroom even
// though archive payloads are streamed.
constexpr std::size_t InnerHeapSize = 16 * 1024 * 1024;
constexpr std::uint64_t ProgressInterval = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t BackupAvailabilityRetryDelaySeconds = 1;
constexpr std::size_t BackupAvailabilityRetryLimit = 5;
constexpr std::uint32_t BackupRetryDelaySeconds = 60;

bool gTimeInitialized = false;

bool isTransientNetworkCurlCode(const int code) {
    switch (static_cast<CURLcode>(code)) {
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_GOT_NOTHING:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
            return true;
        default:
            return false;
    }
}

std::string preflightOutcomeKey(const nxsync::PreflightOutcome outcome) {
    switch (outcome) {
        case nxsync::PreflightOutcome::NoCloudRevision:
            return "no-cloud-revision";
        case nxsync::PreflightOutcome::Synchronized:
            return "synchronized";
        case nxsync::PreflightOutcome::LocalNewer:
            return "local-newer";
        case nxsync::PreflightOutcome::CloudUpdateAvailable:
            return "cloud-update-available";
        case nxsync::PreflightOutcome::Conflict:
            return "conflict";
        case nxsync::PreflightOutcome::InvalidCloudIndex:
        default:
            return "invalid-cloud-index";
    }
}

bool ensureDirectories() {
    if (mkdir("sdmc:/config", 0777) != 0 && errno != EEXIST) return false;
    return mkdir("sdmc:/config/NXSync", 0777) == 0 || errno == EEXIST;
}

void notifyNxsync(
    const std::string& titleId,
    const std::string& titleName,
    const std::string& message,
    const int priority = 20,
    const unsigned durationMs = 4500) {
    nxsync::UltrahandNotification notification;
    notification.title = "NXSync";
    notification.text = (titleName.empty()
        ? "Title " + titleId
        : titleName) + ": " + message;
    notification.priority = priority;
    notification.durationMs = durationMs;
    int systemError = 0;
    nxsync::postUltrahandNotification(
        NotificationDirectory,
        "NXSync",
        notification,
        armGetSystemTick(),
        systemError);
}

bool writeStatusAtomic(const nxsync::CloudWorkerStatus& status) {
    if (!ensureDirectories()) return false;
    int systemError = 0;
    return nxsync::writeTextFileAtomic(
        StatusPath,
        nxsync::serializeCloudWorkerStatus(status),
        systemError);
}

bool waitForInternetConnection(
    nxsync::CloudWorkerStatus& status,
    const std::uint32_t maximumWaitMs,
    Result& lastResult) {
    lastResult = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(lastResult)) return false;

    std::uint32_t elapsedMs = 0;
    while (true) {
        NifmInternetConnectionType connectionType{};
        NifmInternetConnectionStatus connectionStatus{};
        u32 wifiStrength = 0;
        lastResult = nifmGetInternetConnectionStatus(
            &connectionType,
            &wifiStrength,
            &connectionStatus);
        if (R_SUCCEEDED(lastResult)
            && connectionStatus == NifmInternetConnectionStatus_Connected) {
            nifmExit();
            return true;
        }
        if (elapsedMs >= maximumWaitMs) break;

        status.state = "network-waiting";
        status.message = "Waiting for Internet connection ("
            + std::to_string(elapsedMs / 1000) + "/"
            + std::to_string(maximumWaitMs / 1000) + " s)";
        status.bytesTransferred = 0;
        status.totalBytes = 0;
        writeStatusAtomic(status);
        const std::uint32_t remaining = maximumWaitMs - elapsedMs;
        const std::uint32_t waitMs = std::min(
            remaining, nxsync::CloudWorkerNetworkPollIntervalMs);
        svcSleepThread(static_cast<s64>(waitMs) * 1'000'000LL);
        elapsedMs += waitMs;
    }
    nifmExit();
    return false;
}

struct LocalBackupProgressContext {
    nxsync::CloudWorkerStatus* status{nullptr};
    std::string titleId;
    std::uint64_t lastPublishedBytes{0};
};

void publishLocalBackupProgress(
    const nxsync::BackupProgress& progress,
    void* contextPointer) {
    auto* context = static_cast<LocalBackupProgressContext*>(contextPointer);
    if (context == nullptr || context->status == nullptr) return;
    const bool stageChanged = context->status->message
        != "Local backup " + progress.stage + " " + context->titleId;
    const bool bytesAdvanced = progress.bytesProcessed
        >= context->lastPublishedBytes + ProgressInterval;
    const bool finished = progress.totalBytes > 0
        && progress.bytesProcessed >= progress.totalBytes;
    context->status->state = "backup-working";
    context->status->message = "Local backup " + progress.stage
        + " " + context->titleId;
    context->status->bytesTransferred = progress.bytesProcessed;
    context->status->totalBytes = progress.totalBytes;
    if (!stageChanged && !bytesAdvanced && !finished) return;
    context->lastPublishedBytes = progress.bytesProcessed;
    writeStatusAtomic(*context->status);
}

bool parseProgramId(const std::string& value, u64& programId) {
    if (value.size() != 16) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 16);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    programId = static_cast<u64>(parsed);
    return true;
}

bool noGameRunning(void*) {
    // The worker itself owns the transient application resource slot. The
    // sysmodule only launches it after the observed game has exited.
    return false;
}

bool processReadyLocalBackup(
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    nxsync::CloudWorkerStatus& status,
    bool& attempted) {
    attempted = false;
    const std::string environment = nxsync::storageEnvironmentKey(
        nxsync::detectCurrentStorageEnvironment());
    const std::uint64_t nowNs = armTicksToNs(armGetSystemTick());
    const std::vector<nxsync::PendingBackupRequest> requests =
        nxsync::loadPendingBackupRequests(BackupRequestRoot, environment);
    const auto ready = std::find_if(
        requests.begin(), requests.end(), [&](const auto& request) {
            return nxsync::isBackupRequestReady(request, nowNs);
        });
    if (ready == requests.end()) return true;

    nxsync::PendingLocalResolution localResolution;
    std::string localResolutionError;
    const std::string requestEnvironment = ready->storageEnvironment.empty()
        ? environment
        : ready->storageEnvironment;
    const bool hasLocalResolution = nxsync::loadPendingLocalResolution(
            LocalResolutionPath, localResolution, localResolutionError)
        && localResolution.storageEnvironment == requestEnvironment
        && localResolution.titleId == ready->titleId;

    attempted = true;
    status.state = "backup-working";
    status.message = "Creating local backup " + ready->titleId;
    status.bytesTransferred = 0;
    status.totalBytes = 0;
    writeStatusAtomic(status);
    notifyNxsync(ready->titleId, {}, "creating local backup");

    u64 programId = 0;
    nxsync::HeadlessBackupResult result;
    if (parseProgramId(ready->titleId, programId)) {
        LocalBackupProgressContext progress{&status, ready->titleId, 0};
        result = nxsync::createHeadlessBackupsForTitle(
            identity,
            programId,
            QueueRoot,
            config.remoteRoot,
            ready->storageEnvironment.empty()
                ? environment
                : ready->storageEnvironment,
            config.nextcloudConfigured(),
            noGameRunning,
            nullptr,
            publishLocalBackupProgress,
            &progress,
            hasLocalResolution ? &localResolution : nullptr,
            config.retentionCount);
    } else {
        result.message = "Invalid request title ID";
    }

    int queueError = 0;
    status.bytesTransferred = 0;
    status.totalBytes = 0;
    if (result.localResolutionApplied
        && !nxsync::completePendingLocalResolution(
            LocalResolutionPath, localResolution.sequence, queueError)) {
        status.state = "error";
        status.message =
            "Backup succeeded, but conflict resolution was not removed (errno "
            + std::to_string(queueError) + ")";
        ++status.failed;
        writeStatusAtomic(status);
        return false;
    }
    if (result.success) {
        if (!nxsync::completeBackupRequest(
                BackupRequestRoot, *ready, queueError)) {
            status.state = "error";
            status.message = "Backup succeeded, but the request was not removed (errno "
                + std::to_string(queueError) + ")";
            ++status.failed;
            writeStatusAtomic(status);
            return false;
        }
        ++status.completed;
        status.state = "completed";
        status.message = result.createdArchives > 0
            ? "Local backup created"
            : (result.unchangedSaves > 0
                ? "Save data unchanged"
                : "No save data to copy");
        if (result.localPruned > 0) {
            status.message += "; removed "
                + std::to_string(result.localPruned) + " old local ZIP files";
        }
        if (result.retentionFailed > 0) {
            status.message += "; local retention was not completed";
        }
        writeStatusAtomic(status);
        std::string localNotice = result.createdArchives > 0
            ? (result.queuedCloudUploads > 0
                ? "backup created; cloud upload queued"
                : "local backup created")
            : (result.unchangedSaves > 0
                ? "save data unchanged; no new backup needed"
                : "no save data to copy");
        if (result.localPruned > 0) {
            localNotice += "; removed "
                + std::to_string(result.localPruned)
                + " old local backups";
        }
        notifyNxsync(
            ready->titleId,
            {},
            localNotice);
        if (result.retentionFailed > 0) {
            notifyNxsync(
                ready->titleId,
                {},
                "backup completed; local retention failed",
                30,
                6500);
        }
        return true;
    }

    const std::string failure = result.message.empty()
        ? "Local backup failed"
        : result.message;
    const bool quickRetry = result.transientUnavailable
        && ready->attemptCount < BackupAvailabilityRetryLimit;
    const std::uint32_t retryDelay = quickRetry
        ? BackupAvailabilityRetryDelaySeconds
        : BackupRetryDelaySeconds;
    if (!nxsync::recordBackupRequestFailure(
            BackupRequestRoot,
            *ready,
            failure,
            nowNs,
            retryDelay,
            queueError)) {
        status.message = failure + " (retry was not recorded, errno "
            + std::to_string(queueError) + ")";
    } else {
        status.message = failure;
    }
    status.state = "error";
    ++status.failed;
    writeStatusAtomic(status);
    if (!quickRetry || ready->attemptCount == 0) {
        notifyNxsync(
            ready->titleId,
            {},
            quickRetry
                ? "save data is still busy; retrying in one second"
                : status.message,
            quickRetry ? 20 : 30,
            quickRetry ? 4500 : 6500);
    }
    return false;
}

std::string fileNameFromPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string parentPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

struct RemoteRetentionResult {
    std::size_t removed{0};
    std::size_t failed{0};
    std::string firstError;
};

RemoteRetentionResult applyVerifiedRemoteRetention(
    nxsync::NextcloudClient& client,
    const std::string& verifiedRemotePath,
    const std::string& profileUid,
    const std::size_t retentionCount,
    nxsync::CloudWorkerStatus& status) {
    RemoteRetentionResult result;
    if (retentionCount == 0 || verifiedRemotePath.empty()) return result;

    const std::string remoteDirectory = parentPath(verifiedRemotePath);
    if (!nxsync::isProfileBackupDirectory(remoteDirectory, profileUid)) return result;
    status.state = "retention-working";
    status.message = "Cloud retention: keeping "
        + std::to_string(retentionCount) + " backups";
    status.bytesTransferred = 0;
    status.totalBytes = 0;
    writeStatusAtomic(status);

    const nxsync::NextcloudListResult listing =
        client.listDirectory(remoteDirectory);
    if (!listing.success) {
        result.failed = 1;
        result.firstError = listing.message;
        return result;
    }

    std::vector<std::string> filenames;
    for (const nxsync::NextcloudEntry& entry : listing.entries) {
        if (!entry.directory) filenames.push_back(entry.name);
    }
    const std::vector<std::string> toDelete = nxsync::selectBackupsToPrune(
        filenames,
        retentionCount,
        fileNameFromPath(verifiedRemotePath));
    for (const std::string& filename : toDelete) {
        const auto entry = std::find_if(
            listing.entries.begin(), listing.entries.end(),
            [&](const nxsync::NextcloudEntry& candidate) {
                return !candidate.directory && candidate.name == filename;
            });
        if (entry == listing.entries.end()) continue;
        status.message = "Deleting old cloud backup - " + filename;
        writeStatusAtomic(status);
        const nxsync::NextcloudResult deletion =
            client.deleteFile(entry->remotePath);
        if (deletion.success) {
            ++result.removed;
        } else {
            ++result.failed;
            if (result.firstError.empty()) result.firstError = deletion.message;
        }
    }
    return result;
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

std::string archiveCreatedUtc(const nxsync::LocalBackupState& state) {
    if (!state.createdUtc.empty()) return state.createdUtc;
    const std::string filename = fileNameFromPath(state.archivePath);
    if (filename.size() >= 18 && filename[4] == '-' && filename[7] == '-'
        && filename[10] == 'T' && filename[17] == 'Z') {
        return filename.substr(0, 18);
    }
    return {};
}

nxsync::SyncUploadPlan makePlan(
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::UserSaves& user,
    const nxsync::SaveEntry& save,
    const nxsync::LocalBackupState& state) {
    nxsync::SyncSaveDescriptor descriptor;
    descriptor.remoteRoot = config.remoteRoot;
    descriptor.deviceId = identity.folderName;
    descriptor.profileName = user.nickname;
    descriptor.profileUid = formatUid(user.uid);
    descriptor.titleId = nxsync::formatTitleId(save.applicationId);
    descriptor.titleName = save.titleName;
    descriptor.gameVersion = save.gameVersion;
    descriptor.saveDataId = nxsync::formatTitleId(save.saveDataId);

    nxsync::SyncRevisionDescriptor revision;
    revision.archivePath = state.archivePath;
    revision.archiveSha256 = state.sha256;
    revision.revisionId = state.revisionId;
    revision.parentRevisionId = state.parentRevisionId;
    revision.parentRevisionIds = state.parentRevisionIds;
    revision.payloadSha256 = state.payloadSha256;
    revision.createdUtc = archiveCreatedUtc(state);
    revision.archiveSize = state.archiveSize;
    revision.uncompressedBytes = state.uncompressedBytes != 0
        ? state.uncompressedBytes : save.rawSize;
    revision.fileCount = state.fileCount;
    revision.markLocalState = save.extraDataAvailable;
    return nxsync::SyncEngine(QueueRoot).planUpload(descriptor, revision);
}

struct ProgressContext {
    nxsync::CloudWorkerStatus* status{nullptr};
    std::uint64_t lastPublishedBytes{0};
};

void publishProgress(const nxsync::NextcloudProgress& progress, void* pointer) {
    auto* context = static_cast<ProgressContext*>(pointer);
    if (context == nullptr || context->status == nullptr) return;
    context->status->state = "upload-working";
    context->status->message = progress.stage + (progress.remotePath.empty()
        ? std::string() : " - " + progress.remotePath);
    context->status->bytesTransferred = progress.bytesTransferred;
    context->status->totalBytes = progress.totalBytes;
    if (progress.bytesTransferred < context->lastPublishedBytes + ProgressInterval
        && progress.bytesTransferred != progress.totalBytes) return;
    context->lastPublishedBytes = progress.bytesTransferred;
    writeStatusAtomic(*context->status);
}

class WorkerTransport final : public nxsync::SyncCloudTransport {
public:
    WorkerTransport(nxsync::NextcloudClient& client, ProgressContext& progress)
        : client_(client), progress_(progress) {}

    nxsync::SyncTransferResult uploadArchive(
        const std::string& localPath,
        const std::string& remotePath,
        const std::string& sha256) override {
        const nxsync::NextcloudResult source = client_.uploadVerified(
            localPath, remotePath, sha256, publishProgress, &progress_);
        return convert(source);
    }

    nxsync::SyncTransferResult uploadDocument(
        const std::string& text,
        const std::string& remotePath) override {
        if (progress_.status != nullptr) {
            progress_.status->state = "index-working";
            progress_.status->message = "Publishing index - " + remotePath;
            progress_.status->bytesTransferred = 0;
            progress_.status->totalBytes = text.size();
            writeStatusAtomic(*progress_.status);
        }
        const nxsync::NextcloudResult source = client_.uploadTextVerified(
            text, remotePath, publishProgress, &progress_);
        return convert(source);
    }

    nxsync::SyncDocumentResult downloadDocument(
        const std::string& remotePath) override {
        const nxsync::NextcloudTextResult source = client_.downloadText(remotePath);
        lastCurlCode_ = source.curlCode;
        lastHttpStatus_ = source.httpStatus;
        nxsync::SyncDocumentResult result;
        result.success = source.success;
        result.message = source.message;
        result.text = source.text;
        return result;
    }

    void resetLastResult() {
        lastCurlCode_ = 0;
        lastHttpStatus_ = 0;
    }

    int lastCurlCode() const {
        return lastCurlCode_;
    }

    long lastHttpStatus() const {
        return lastHttpStatus_;
    }

private:
    nxsync::SyncTransferResult convert(const nxsync::NextcloudResult& source) {
        lastCurlCode_ = source.curlCode;
        lastHttpStatus_ = source.httpStatus;
        return {
            source.success,
            source.success
                ? source.message
                : nxsync::formatNextcloudFailure(source)};
    }

    nxsync::NextcloudClient& client_;
    ProgressContext& progress_;
    int lastCurlCode_{0};
    long lastHttpStatus_{0};
};

struct MarkContext {
    const nxsync::DeviceIdentity* identity{nullptr};
    const nxsync::UserSaves* user{nullptr};
    const nxsync::SaveEntry* save{nullptr};
};

bool markUploaded(
    const std::string& remotePath,
    void* pointer,
    std::string& error) {
    const auto* context = static_cast<const MarkContext*>(pointer);
    if (context == nullptr || context->identity == nullptr
        || context->user == nullptr || context->save == nullptr) {
        error = "Invalid local status context";
        return false;
    }
    int systemError = 0;
    if (!nxsync::markLocalBackupUploaded(
            *context->identity,
            *context->user,
            *context->save,
            remotePath,
            systemError)) {
        error = "Cloud verified, but local status was not updated (errno "
            + std::to_string(systemError) + ")";
        return false;
    }
    error.clear();
    return true;
}

bool findQueuedSave(
    const nxsync::SaveCatalog& catalog,
    const nxsync::PendingCloudOperation& operation,
    const nxsync::UserSaves*& matchedUser,
    const nxsync::SaveEntry*& matchedSave) {
    matchedUser = nullptr;
    matchedSave = nullptr;
    for (const nxsync::UserSaves& user : catalog.users) {
        if (formatUid(user.uid) != operation.profileUid) continue;
        const auto save = std::find_if(
            user.saves.begin(),
            user.saves.end(),
            [&](const nxsync::SaveEntry& candidate) {
                return nxsync::formatTitleId(candidate.applicationId)
                        == operation.titleId
                    && nxsync::formatTitleId(candidate.saveDataId)
                        == operation.saveDataId;
            });
        if (save != user.saves.end()) {
            matchedUser = &user;
            matchedSave = &*save;
            return true;
        }
    }
    return false;
}

bool findPreflightSave(
    const nxsync::SaveCatalog& catalog,
    const nxsync::PreflightRequest& request,
    const nxsync::UserSaves*& matchedUser,
    const nxsync::SaveEntry*& matchedSave) {
    matchedUser = nullptr;
    matchedSave = nullptr;
    for (const nxsync::UserSaves& user : catalog.users) {
        if (!request.profileUid.empty() && formatUid(user.uid) != request.profileUid) {
            continue;
        }
        if (request.profileUid.empty() && !user.registeredProfile) continue;
        const auto save = std::find_if(
            user.saves.begin(),
            user.saves.end(),
            [&](const nxsync::SaveEntry& candidate) {
                return nxsync::formatTitleId(candidate.applicationId)
                    == request.titleId;
            });
        if (save == user.saves.end()) continue;
        if (matchedUser != nullptr) {
            // The automatic gate must not guess which profile to use.
            matchedUser = nullptr;
            matchedSave = nullptr;
            return false;
        }
        matchedUser = &user;
        matchedSave = &*save;
        if (!request.profileUid.empty()) return true;
    }
    return matchedUser != nullptr;
}

void publishPreflightError(
    const nxsync::PreflightRequest& request,
    const std::string& message) {
    nxsync::PreflightStatus status;
    status.workerBuildVersion = BuildVersion;
    status.sequence = request.sequence;
    status.titleId = request.titleId;
    status.profileUid = request.profileUid;
    status.state = "error";
    status.message = message;
    int systemError = 0;
    nxsync::writePreflightStatusAtomic(
        PreflightStatusPath,
        status,
        systemError);
}

void completePreflight(const nxsync::PreflightRequest& request) {
    int systemError = 0;
    nxsync::completePreflightRequest(
        PreflightRequestPath,
        request.sequence,
        systemError);
}

bool runPreflight(
    nxsync::NextcloudClient& client,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog,
    const nxsync::PreflightRequest& request) {
    nxsync::PreflightStatus status;
    status.workerBuildVersion = BuildVersion;
    status.sequence = request.sequence;
    status.titleId = request.titleId;
    status.profileUid = request.profileUid;
    status.state = "working";
    status.message = "Reading global index";
    int systemError = 0;
    nxsync::writePreflightStatusAtomic(PreflightStatusPath, status, systemError);

    const nxsync::UserSaves* user = nullptr;
    const nxsync::SaveEntry* save = nullptr;
    if (!findPreflightSave(catalog, request, user, save)) {
        publishPreflightError(
            request,
            request.automaticLaunch && request.profileUid.empty()
                ? "Profile selection is required for automatic launch"
                : "Profile or local save unavailable for preflight");
        completePreflight(request);
        return false;
    }
    status.profileUid = formatUid(user->uid);

    const std::string normalizedRoot = nxsync::normalizeRemoteRoot(
        config.remoteRoot);
    const std::string titleRoot = normalizedRoot
        + "/_index/titles/" + request.titleId;
    const nxsync::NextcloudListResult headListing = client.listDirectory(
        titleRoot);
    if (!headListing.success && headListing.httpStatus != 404) {
        publishPreflightError(request, headListing.message);
        completePreflight(request);
        return false;
    }

    std::vector<nxsync::CloudIndexEntry> heads;
    std::size_t invalidDocuments = 0;
    if (headListing.success) {
        for (const nxsync::NextcloudEntry& remote : headListing.entries) {
            if (remote.directory || remote.name.size() < 6
                || remote.name.substr(remote.name.size() - 5) != ".json") {
                continue;
            }
            const nxsync::NextcloudTextResult downloaded =
                client.downloadText(remote.remotePath);
            nxsync::CloudIndexEntry entry;
            std::string error;
            if (downloaded.success
                && nxsync::parseCloudIndexEntry(downloaded.text, entry, error)
                && entry.titleId == request.titleId) {
                heads.push_back(std::move(entry));
            } else {
                ++invalidDocuments;
            }
        }
    }

    const nxsync::LocalBackupState local = nxsync::findCurrentLocalBackup(
        identity,
        *user,
        *save);
    const nxsync::RestoreLineageAnchor restoreAnchor =
        nxsync::loadRestoreLineageAnchor(
            identity,
            *user,
            save->applicationId);
    const nxsync::RevisionNode localNode = local.current
        ? nxsync::RevisionNode{
            local.revisionId,
            local.parentRevisionId,
            local.payloadSha256,
            local.parentRevisionIds}
        : nxsync::restoredRevisionNode(restoreAnchor);
    std::vector<nxsync::RevisionNode> history;
    nxsync::PreflightDecision decision = nxsync::evaluatePreflight(
        localNode, heads, history);

    // Same revision, same payload and direct parent/child relationships are
    // fully described by the head documents.  Only ambiguous relationships
    // need the complete revision graph, which avoids one WebDAV GET per old
    // revision on the normal launch path.
    if (decision.outcome == nxsync::PreflightOutcome::Conflict) {
        status.message = "Checking revision history";
        nxsync::writePreflightStatusAtomic(
            PreflightStatusPath, status, systemError);
        std::set<std::string> visitedRevisions;
        std::vector<std::string> pendingRevisions;
        const auto rememberKnown = [&](const nxsync::RevisionNode& node) {
            if (!node.revisionId.empty()) {
                visitedRevisions.insert(node.revisionId);
            }
        };
        const auto queueRevision = [&](const std::string& revisionId) {
            if (!revisionId.empty()
                && visitedRevisions.insert(revisionId).second) {
                pendingRevisions.push_back(revisionId);
            }
        };
        rememberKnown(localNode);
        for (const nxsync::CloudIndexEntry& head : heads) {
            rememberKnown(nxsync::RevisionNode{
                head.revisionId,
                head.parentRevisionId,
                head.payloadSha256,
                head.parentRevisionIds});
        }
        for (const std::string& parent : nxsync::revisionParents(localNode)) {
            queueRevision(parent);
        }
        for (const nxsync::CloudIndexEntry& head : heads) {
            const nxsync::RevisionNode headNode{
                head.revisionId,
                head.parentRevisionId,
                head.payloadSha256,
                head.parentRevisionIds};
            for (const std::string& parent : nxsync::revisionParents(headNode)) {
                queueRevision(parent);
            }
        }

        // Revision files are content-addressed by revision ID. Follow only the
        // ancestor chains of the local node and current cloud heads instead of
        // listing and downloading every historical branch for the title.
        for (std::size_t cursor = 0;
             cursor < pendingRevisions.size() && cursor < 1024;
             ++cursor) {
            const std::string& revisionId = pendingRevisions[cursor];
            const nxsync::NextcloudTextResult downloaded = client.downloadText(
                nxsync::makeCloudRevisionRemotePath(
                    config.remoteRoot, request.titleId, revisionId));
            nxsync::CloudIndexEntry entry;
            std::string error;
            if (downloaded.success
                && nxsync::parseCloudIndexEntry(downloaded.text, entry, error)
                && entry.titleId == request.titleId
                && entry.revisionId == revisionId) {
                history.push_back(nxsync::RevisionNode{
                    entry.revisionId,
                    entry.parentRevisionId,
                    entry.payloadSha256,
                    entry.parentRevisionIds});
                for (const std::string& parent : nxsync::revisionParents(
                         history.back())) {
                    queueRevision(parent);
                }
                decision = nxsync::evaluatePreflight(
                    localNode, heads, history);
                if (decision.outcome != nxsync::PreflightOutcome::Conflict) {
                    break;
                }
            }
        }
        decision = nxsync::evaluatePreflight(localNode, heads, history);
    }

    status.state = "completed";
    status.outcome = preflightOutcomeKey(decision.outcome);
    status.message = decision.message;
    status.validHeads = decision.validHeads;
    status.invalidHeads = decision.invalidHeads + invalidDocuments;
    status.localRevisionId = localNode.revisionId;
    status.localPayloadSha256 = localNode.payloadSha256;
    const std::vector<std::size_t> restoreChoiceIndices =
        nxsync::cloudRestoreChoiceIndices(localNode, heads);
    for (const std::size_t index : restoreChoiceIndices) {
        const nxsync::CloudIndexEntry& head = heads[index];
        if (head.revisionId.empty() || head.archiveRemotePath.empty()) continue;
        status.candidates.push_back(nxsync::PreflightCandidate{
            head.deviceId,
            head.profileName,
            head.revisionId,
            head.payloadSha256,
            head.archiveRemotePath,
            head.gameVersion,
            head.createdUtc});
    }
    if (decision.hasSelectedHead && decision.selectedHeadIndex < heads.size()) {
        const nxsync::CloudIndexEntry& selected = heads[decision.selectedHeadIndex];
        status.selectedDeviceId = selected.deviceId;
        status.selectedProfileName = selected.profileName;
        status.selectedRevisionId = selected.revisionId;
        status.selectedArchivePath = selected.archiveRemotePath;
        status.selectedGameVersion = selected.gameVersion;
        status.selectedCreatedUtc = selected.createdUtc;
        status.selectedPayloadSha256 = selected.payloadSha256;
    }
    nxsync::writePreflightStatusAtomic(PreflightStatusPath, status, systemError);
    completePreflight(request);
    return true;
}

const nxsync::UserSaves* findLaunchUser(
    const nxsync::SaveCatalog& catalog,
    const nxsync::PreflightStatus& preflight) {
    const auto found = std::find_if(
        catalog.users.begin(), catalog.users.end(),
        [&](const nxsync::UserSaves& user) {
            return user.registeredProfile
                && formatUid(user.uid) == preflight.profileUid;
        });
    return found == catalog.users.end() ? nullptr : &*found;
}

bool finishLaunchRestore(
    const nxsync::LaunchRequest& request,
    const bool restored,
    const std::string& message,
    nxsync::CloudWorkerStatus& workerStatus,
    const bool safeToLaunch = true) {
    nxsync::LaunchDecision decision;
    decision.sequence = request.sequence;
    decision.action = safeToLaunch ? "allow" : "abort";
    decision.message = message;
    int systemError = 0;
    const bool resultWritten = nxsync::writeLaunchDecisionAtomic(
        LaunchWorkerResultPath, decision, systemError);
    int actionError = 0;
    nxsync::completeLaunchAction(
        LaunchActionPath, request.sequence, actionError);
    workerStatus.state = restored ? "completed" : "error";
    workerStatus.message = resultWritten
        ? message
        : "Restore completed, but the worker result was not written (errno "
            + std::to_string(systemError) + ")";
    workerStatus.completed = restored ? 1 : 0;
    workerStatus.failed = restored ? 0 : 1;
    writeStatusAtomic(workerStatus);
    return restored && resultWritten;
}

bool runLaunchRestore(
    nxsync::NextcloudClient& client,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog,
    const nxsync::LaunchRequest& request,
    const nxsync::PreflightStatus& preflight,
    nxsync::CloudWorkerStatus& workerStatus) {
    if (preflight.sequence != request.sequence
        || preflight.titleId != request.titleId
        || preflight.state != "completed"
        || (preflight.outcome != "cloud-update-available"
            && preflight.outcome != "conflict")
        || preflight.selectedArchivePath.empty()) {
        return finishLaunchRestore(
            request,
            false,
            "Invalid cloud revision: launching with the local save",
            workerStatus);
    }

    const nxsync::UserSaves* user = findLaunchUser(catalog, preflight);
    if (user == nullptr) {
        return finishLaunchRestore(
            request,
            false,
            "Local profile unavailable: launching with the local save",
            workerStatus);
    }

    const auto save = std::find_if(
        user->saves.begin(), user->saves.end(),
        [&](const nxsync::SaveEntry& candidate) {
            return nxsync::formatTitleId(candidate.applicationId) == request.titleId;
        });
    if (save == user->saves.end()) {
        return finishLaunchRestore(
            request,
            false,
            "Local save unavailable: launching without restore",
            workerStatus);
    }

    if (nxsync::compareGameVersions(
            save->gameVersion,
            preflight.selectedGameVersion) == nxsync::GameVersionOrder::Older) {
        return finishLaunchRestore(
            request,
            false,
            "Game version is too old: launching with the local save",
            workerStatus);
    }

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/NXSync", 0777);
    mkdir("sdmc:/switch/NXSync/imports", 0777);
    workerStatus.state = "download-working";
    workerStatus.message = "Downloading cloud revision before launch";
    workerStatus.bytesTransferred = 0;
    workerStatus.totalBytes = 0;
    writeStatusAtomic(workerStatus);
    ProgressContext progress{&workerStatus, 0};
    const nxsync::NextcloudResult download = client.downloadVerified(
        preflight.selectedArchivePath,
        LaunchRestoreDownloadPath,
        publishProgress,
        &progress);
    if (!download.success) {
        return finishLaunchRestore(
            request,
            false,
            "Cloud download failed: launching with the local save",
            workerStatus);
    }

    workerStatus.state = "restore-working";
    workerStatus.message = "Verifying archive and restoring save data";
    writeStatusAtomic(workerStatus);
    const nxsync::RestoreInspection inspection =
        nxsync::inspectRestoreArchive(LaunchRestoreDownloadPath);
    if (!inspection.success
        || nxsync::formatTitleId(inspection.manifest.titleId) != request.titleId
        || !nxsync::restoreMatchesSelectedRevision(
            preflight.selectedRevisionId, preflight.selectedPayloadSha256,
            inspection.manifest.revisionId, inspection.manifest.payloadSha256)) {
        return finishLaunchRestore(
            request,
            false,
            "Cloud archive does not match the selected revision; save untouched",
            workerStatus);
    }

    // Download/inspection never grants authority to mutate a save. Only the
    // patched dmnt holding this exact process can issue the durable lease.
    const nxsync::LaunchRestoreLease claim{request, formatUid(user->uid)};
    int leaseError = 0;
    if (!nxsync::writeLaunchRestoreLease(nxsync::LaunchRestoreClaimPath, claim, leaseError)) {
        return finishLaunchRestore(request, false, "Unable to request restore permission", workerStatus);
    }
    const auto leaseStart = armGetSystemTick();
    bool granted = false;
    while (armTicksToNs(armGetSystemTick() - leaseStart) < 10'000'000'000ULL) {
        nxsync::LaunchRestoreLease grant, guard;
        std::string error;
        if (nxsync::loadLaunchRestoreLease(nxsync::LaunchRestoreGrantPath, grant, error)
            && nxsync::loadLaunchRestoreLease(nxsync::LaunchRestoreGuardPath, guard, error)
            && nxsync::serializeLaunchRestoreLease(grant) == nxsync::serializeLaunchRestoreLease(claim)
            && nxsync::serializeLaunchRestoreLease(guard) == nxsync::serializeLaunchRestoreLease(claim)) {
            granted = true;
            break;
        }
        svcSleepThread(100'000'000);
    }
    if (!granted) {
        // The gate may already have timed out and started the game. Do not touch
        // the save, even if a late/stale restore action is still on the SD card.
        return finishLaunchRestore(request, false, "Restore permission expired; save untouched", workerStatus);
    }
    const nxsync::RestoreResult restore = nxsync::restoreArchiveToProfile(
        LaunchRestoreDownloadPath,
        inspection,
        identity,
        *user);
    bool safeToLaunch = nxsync::restoreAllowsLaunch(
        restore.success, restore.destinationModified, restore.recoverySucceeded);
    if (safeToLaunch) {
        safeToLaunch = nxsync::clearRecoveredLaunchGuard(request.titleId, claim.profileUid, leaseError);
    }
    if (!restore.success || !safeToLaunch) {
        return finishLaunchRestore(
            request,
            false,
            safeToLaunch ? "Cloud restore failed; local save recovered"
                : "Launch blocked: save recovery required. Safety archive: " + restore.safetyBackupPath,
            workerStatus,
            safeToLaunch);
    }
    const std::string& selectedRevision = inspection.manifest.revisionId;
    const std::string& selectedPayload = inspection.manifest.payloadSha256;
    std::vector<std::string> resolutionParents{selectedRevision};
    if (preflight.outcome == "conflict"
        && !preflight.localRevisionId.empty()
        && preflight.localRevisionId != selectedRevision) {
        resolutionParents.push_back(preflight.localRevisionId);
    }
    if (preflight.outcome == "conflict") {
        for (const nxsync::PreflightCandidate& candidate : preflight.candidates) {
            if (candidate.revisionId != selectedRevision
                && std::find(
                    resolutionParents.begin(),
                    resolutionParents.end(),
                    candidate.revisionId) == resolutionParents.end()) {
                resolutionParents.push_back(candidate.revisionId);
            }
        }
    }
    int lineageError = 0;
    if (resolutionParents.size() > nxsync::MaximumRevisionParents
        || !nxsync::writeRestoreLineageAnchor(
            identity,
            *user,
            save->applicationId,
            selectedRevision,
            selectedPayload,
            resolutionParents,
            lineageError)) {
        std::remove(LaunchRestoreDownloadPath);
        return finishLaunchRestore(
            request,
            true,
            "Cloud save restored; logical merge was not recorded (errno "
                + std::to_string(lineageError) + ")",
            workerStatus);
    }
    std::remove(LaunchRestoreDownloadPath);
    return finishLaunchRestore(
        request,
        true,
        preflight.outcome == "conflict"
            ? "Cloud save restored; logical merge queued"
            : "Cloud save restored; launch authorized",
        workerStatus);
}

} // namespace

extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;

void __libnx_initheap(void) {
    static u8 innerHeap[InnerHeapSize];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = innerHeap;
    fake_heap_end = innerHeap + sizeof(innerHeap);
}

void __appInit(void) {
    Result result = smInitialize();
    if (R_FAILED(result)) diagAbortWithResult(result);
    result = setsysInitialize();
    if (R_SUCCEEDED(result)) {
        SetSysFirmwareVersion firmware{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware))) {
            hosversionSet(MAKEHOSVERSION(
                firmware.major, firmware.minor, firmware.micro));
        }
        setsysExit();
    }
    result = fsInitialize();
    if (R_FAILED(result)) diagAbortWithResult(result);
    result = fsdevMountSdmc();
    if (R_FAILED(result)) diagAbortWithResult(result);
    gTimeInitialized = R_SUCCEEDED(timeInitialize());
}

void __appExit(void) {
    std::remove(LockPath);
    if (gTimeInitialized) timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

} // extern "C"

int main(int, char**) {
    nxsync::CloudWorkerStatus status;
    status.buildVersion = BuildVersion;
    status.state = "starting";
    status.message = "Initializing cloud worker";
    writeStatusAtomic(status);

    nxsync::LaunchRequest launchRequest;
    std::string launchRequestError;
    const bool hasLaunchRequest = nxsync::loadLaunchRequest(
        LaunchRequestPath, launchRequest, launchRequestError);
    nxsync::LaunchAction launchAction;
    std::string launchActionError;
    const bool hasLaunchRestore = hasLaunchRequest
        && nxsync::loadLaunchAction(
            LaunchActionPath, launchAction, launchActionError)
        && launchAction.sequence == launchRequest.sequence
        && launchAction.action == "restore-cloud";

    if (!gTimeInitialized) {
        status.state = "error";
        status.message = "Date/time service unavailable";
        status.failed = 1;
        writeStatusAtomic(status);
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest, false,
                "Date/time service unavailable: launching locally", status);
        }
        return 0;
    }

    nxsync::PreflightRequest preflightRequest;
    std::string preflightRequestError;
    const bool hasPreflightRequest = nxsync::loadPreflightRequest(
        PreflightRequestPath,
        preflightRequest,
        preflightRequestError);
    const nxsync::AppConfig config = nxsync::loadOrCreateConfig(ConfigPath);
    const nxsync::DeviceIdentity identity = nxsync::detectDeviceIdentity(
        config.deviceIdOverride,
        FallbackIdPath);
    bool localBackupAttempted = false;
    if (!hasPreflightRequest && !hasLaunchRestore
        && !processReadyLocalBackup(
            config, identity, status, localBackupAttempted)) {
        return 0;
    }
    if (!config.nextcloudConfigured()) {
        if (localBackupAttempted) {
            status.state = "completed";
            status.message = "Local backup completed; Nextcloud is not configured";
            writeStatusAtomic(status);
            return 0;
        }
        status.state = "error";
        status.message = config.credentialError.empty()
            ? "Incomplete Nextcloud configuration"
            : config.credentialError;
        status.failed = 1;
        writeStatusAtomic(status);
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest, false,
                config.credentialError.empty()
                    ? "Nextcloud is not configured: launching locally"
                    : "Encrypted credential unavailable: launching locally",
                status);
        }
        return 0;
    }
    nxsync::PreflightStatus launchPreflight;
    std::string launchPreflightError;
    const bool hasLaunchPreflight = hasLaunchRestore
        && nxsync::loadPreflightStatus(
            PreflightStatusPath, launchPreflight, launchPreflightError)
        && launchPreflight.sequence == launchRequest.sequence;
    const nxsync::SaveCatalog catalog = nxsync::loadSaveCatalog();
    if (!catalog.ok()) {
        if (hasPreflightRequest) {
            publishPreflightError(
                preflightRequest,
                "Save catalog unavailable: "
                    + nxsync::formatResult(catalog.saveReaderResult));
            completePreflight(preflightRequest);
        }
        status.state = "error";
        status.message = "Save catalog unavailable: "
            + nxsync::formatResult(catalog.saveReaderResult);
        status.failed = 1;
        writeStatusAtomic(status);
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest, false,
                "Save catalog unavailable: launching locally", status);
        }
        return 0;
    }

    const nxsync::SyncEngine engine(QueueRoot);
    const std::vector<nxsync::PendingCloudOperation> pending =
        engine.pendingOperations();
    if (pending.empty() && !hasPreflightRequest && !hasLaunchRestore) {
        status.state = "completed";
        status.message = localBackupAttempted
            ? "Local backup completed; cloud queue is empty"
            : "Cloud queue is empty";
        writeStatusAtomic(status);
        return 0;
    }

    Result networkStatusResult = 0;
    const bool interactiveNetworkRequest =
        hasPreflightRequest || hasLaunchRestore;
    if (!waitForInternetConnection(
            status,
            nxsync::cloudWorkerNetworkWaitBudgetMs(interactiveNetworkRequest),
            networkStatusResult)) {
        const std::string networkMessage =
            "Internet connection unavailable; cloud operations remain queued";
        if (hasPreflightRequest) {
            publishPreflightError(preflightRequest, networkMessage);
            completePreflight(preflightRequest);
        }
        status.state = "network-unavailable";
        status.message = networkMessage;
        status.failed = 0;
        status.bytesTransferred = 0;
        status.totalBytes = 0;
        writeStatusAtomic(status);
        if (!pending.empty()) {
            notifyNxsync(
                pending.front().titleId,
                {},
                "network unavailable; upload remains queued",
                20,
                5500);
        }
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest,
                false,
                "Network unavailable: launching with the local save",
                status);
        }
        return 0;
    }

    const Result sslProbeResult = sslInitialize(3);
    if (R_FAILED(sslProbeResult)) {
        if (hasPreflightRequest) {
            publishPreflightError(
                preflightRequest,
                "SSL service unavailable: "
                    + nxsync::formatResult(sslProbeResult));
            completePreflight(preflightRequest);
        }
        status.state = "error";
        status.message = "SSL service unavailable: "
            + nxsync::formatResult(sslProbeResult);
        status.failed = pending.size();
        writeStatusAtomic(status);
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest, false,
                "SSL service unavailable: launching locally", status);
        }
        return 0;
    }
    const Result csrngProbeResult = csrngInitialize();
    if (R_FAILED(csrngProbeResult)) {
        sslExit();
        if (hasPreflightRequest) {
            publishPreflightError(
                preflightRequest,
                "CSRNG service unavailable: "
                    + nxsync::formatResult(csrngProbeResult));
            completePreflight(preflightRequest);
        }
        status.state = "error";
        status.message = "CSRNG service unavailable: "
            + nxsync::formatResult(csrngProbeResult);
        status.failed = pending.size();
        writeStatusAtomic(status);
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest, false,
                "Random service unavailable: launching locally", status);
        }
        return 0;
    }
    csrngExit();
    sslExit();

    nxsync::NextcloudClient client(config);
    if (!client.ready()) {
        if (hasPreflightRequest) {
            publishPreflightError(
                preflightRequest,
                client.initializationResult().message);
            completePreflight(preflightRequest);
        }
        status.state = "error";
        status.message = client.initializationResult().message;
        status.failed = pending.size();
        for (const auto& operation : pending) {
            int queueError = 0;
            engine.recordFailure(operation, status.message, queueError);
        }
        writeStatusAtomic(status);
        if (hasLaunchRestore) {
            finishLaunchRestore(
                launchRequest, false,
                "Cloud worker was not initialized: launching locally", status);
        }
        return 0;
    }

    if (hasLaunchRestore) {
        if (!hasLaunchPreflight) {
            finishLaunchRestore(
                launchRequest,
                false,
                "Cloud result unavailable: launching with the local save",
                status);
            return 0;
        }
        if (!launchAction.selectedRevisionId.empty()) {
            const auto selected = std::find_if(
                launchPreflight.candidates.begin(),
                launchPreflight.candidates.end(),
                [&](const nxsync::PreflightCandidate& candidate) {
                    return candidate.revisionId
                        == launchAction.selectedRevisionId;
                });
            if (selected == launchPreflight.candidates.end()) {
                finishLaunchRestore(
                    launchRequest,
                    false,
                    "Selected cloud revision unavailable: launching locally",
                    status);
                return 0;
            }
            launchPreflight.selectedDeviceId = selected->deviceId;
            launchPreflight.selectedProfileName = selected->profileName;
            launchPreflight.selectedRevisionId = selected->revisionId;
            launchPreflight.selectedArchivePath = selected->archivePath;
            launchPreflight.selectedGameVersion = selected->gameVersion;
            launchPreflight.selectedCreatedUtc = selected->createdUtc;
            launchPreflight.selectedPayloadSha256 = selected->payloadSha256;
        }
        runLaunchRestore(
            client,
            identity,
            catalog,
            launchRequest,
            launchPreflight,
            status);
        return 0;
    }

    if (hasPreflightRequest) {
        const bool preflightSucceeded = runPreflight(
            client,
            config,
            identity,
            catalog,
            preflightRequest);
        if (pending.empty()) {
            status.state = preflightSucceeded ? "completed" : "error";
            status.message = preflightSucceeded
                ? "Cloud preflight completed"
                : "Cloud preflight failed";
            status.failed = preflightSucceeded ? 0 : 1;
            writeStatusAtomic(status);
            return 0;
        }
    }

    ProgressContext progress{&status, 0};
    WorkerTransport transport(client, progress);
    std::size_t remotePruned = 0;
    std::size_t retentionFailed = 0;
    std::string retentionFirstError;
    for (const nxsync::PendingCloudOperation& operation : pending) {
        if (nxsync::launchRestoreBlocksTitle(operation.titleId)) {
            ++status.failed;
            status.state = "recovery-required";
            status.message = "Save recovery required; cloud upload and retention are paused";
            writeStatusAtomic(status);
            continue;
        }
        status.state = "upload-working";
        status.revisionId = operation.revisionId;
        status.message = "Preparing upload " + operation.titleId;
        status.bytesTransferred = 0;
        status.totalBytes = 0;
        progress.lastPublishedBytes = 0;
        writeStatusAtomic(status);

        const nxsync::UserSaves* user = nullptr;
        const nxsync::SaveEntry* save = nullptr;
        std::string failure;
        bool failureAlreadyRecorded = false;
        if (!findQueuedSave(catalog, operation, user, save)) {
            failure = "Operation has no profile or local save";
        } else {
            const nxsync::LocalBackupState local =
                nxsync::findCurrentLocalBackup(identity, *user, *save);
            const nxsync::SyncUploadPlan plan = makePlan(
                config, identity, *user, *save, local);
            const nxsync::PendingOperationMatch match =
                engine.matchPendingOperation(operation, plan);
            if (!local.recordValid) {
                failure = "Current local backup not found for "
                    + identity.folderName;
            } else if (match != nxsync::PendingOperationMatch::Match) {
                failure = match == nxsync::PendingOperationMatch::DifferentSave
                    ? "Queue is associated with a different local save"
                    : (match == nxsync::PendingOperationMatch::InvalidPlan
                        ? "Invalid cloud plan: " + plan.error
                        : "Queue refers to an older local revision");
            } else {
                MarkContext mark{&identity, user, save};
                nxsync::SyncExecutionResult execution;
                bool archiveAlreadyVerified = false;
                for (std::size_t attempt = 0;
                     attempt < nxsync::CloudTransferAttemptLimit;
                     ++attempt) {
                    transport.resetLastResult();
                    progress.lastPublishedBytes = 0;
                    execution = engine.execute(
                        plan,
                        transport,
                        archiveAlreadyVerified,
                        save->extraDataAvailable ? markUploaded : nullptr,
                        save->extraDataAvailable ? &mark : nullptr,
                        true);
                    archiveAlreadyVerified = archiveAlreadyVerified
                        || execution.archiveUploaded;
                    const bool transientCurl = isTransientNetworkCurlCode(
                        transport.lastCurlCode());
                    const bool transientHttp =
                        nxsync::cloudHttpStatusIsTransient(
                            transport.lastHttpStatus());
                    if (execution.success
                        || (!transientCurl && !transientHttp)
                        || attempt + 1 >= nxsync::CloudTransferAttemptLimit) {
                        break;
                    }
                    const std::uint32_t retryDelaySeconds = transientHttp
                        ? nxsync::cloudHttpRetryDelaySeconds(
                            transport.lastHttpStatus(), attempt)
                        : nxsync::cloudTransferRetryDelaySeconds(attempt);
                    status.state = "network-retry";
                    status.message = transientHttp
                        ? "HTTP " + std::to_string(transport.lastHttpStatus())
                            + " is transient; attempt "
                        : "Transient network error; attempt ";
                    status.message +=
                        std::to_string(attempt + 2) + "/"
                        + std::to_string(nxsync::CloudTransferAttemptLimit)
                        + " in " + std::to_string(retryDelaySeconds) + " s";
                    status.bytesTransferred = 0;
                    status.totalBytes = 0;
                    writeStatusAtomic(status);
                    svcSleepThread(
                        static_cast<s64>(retryDelaySeconds)
                        * 1'000'000'000LL);
                    status.state = "upload-working";
                    status.message = "Retrying upload "
                        + operation.titleId;
                    writeStatusAtomic(status);
                }
                if (execution.success) {
                    ++status.completed;
                    const RemoteRetentionResult retention =
                        applyVerifiedRemoteRetention(
                            client,
                            execution.remotePath,
                            operation.profileUid,
                            config.retentionCount,
                            status);
                    remotePruned += retention.removed;
                    retentionFailed += retention.failed;
                    if (retentionFirstError.empty()
                        && !retention.firstError.empty()) {
                        retentionFirstError = retention.firstError;
                    }
                    notifyNxsync(
                        operation.titleId,
                        save->titleName,
                        retention.failed > 0
                            ? "upload verified; cloud retention failed"
                            : (retention.removed > 0
                                ? "upload verified; removed "
                                    + std::to_string(retention.removed)
                                    + " old cloud backups"
                                : "cloud upload completed and verified"),
                        retention.failed > 0 ? 30 : 20,
                        retention.failed > 0 ? 6500 : 4500);
                } else {
                    failure = execution.message;
                    failureAlreadyRecorded = true;
                }
            }
        }

        if (!failure.empty()) {
            ++status.failed;
            status.state = "error";
            status.message = failure;
            if (!failureAlreadyRecorded) {
                int queueError = 0;
                engine.recordFailure(operation, failure, queueError);
            }
            writeStatusAtomic(status);
            notifyNxsync(
                operation.titleId,
                save == nullptr ? std::string() : save->titleName,
                "cloud upload failed: " + failure,
                30,
                7000);
            break;
        }
    }

    if (status.failed == 0) {
        status.state = "completed";
        status.message = "Cloud uploads completed and verified";
        if (remotePruned > 0) {
            status.message += "; removed "
                + std::to_string(remotePruned) + " old ZIP files";
        }
        if (retentionFailed > 0) {
            status.message += "; retention was not completed: "
                + retentionFirstError;
        }
        status.revisionId.clear();
    }
    writeStatusAtomic(status);
    return 0;
}
