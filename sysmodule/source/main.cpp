#include <switch.h>

#include "nxsync/atomic_file.hpp"
#include "nxsync/application_lifecycle.hpp"
#include "nxsync/backup_request_probe.hpp"
#include "nxsync/backup_request_queue.hpp"
#include "nxsync/cloud_queue.hpp"
#include "nxsync/cloud_worker_policy.hpp"
#include "nxsync/launch_gate_policy.hpp"
#include "nxsync/launch_protocol.hpp"
#include "nxsync/local_resolution.hpp"
#include "nxsync/overlay_catalog.hpp"
#include "nxsync/overlay_dependencies.hpp"
#include "nxsync/preflight_protocol.hpp"
#include "nxsync/revision_parents.hpp"
#include "nxsync/storage_environment.hpp"
#include "nxsync/sysmodule_config.hpp"
#include "nxsync/sysmodule_status.hpp"
#include "nxsync/ultrahand_notification.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char* BuildVersion = "0.14.6-rc1";
constexpr u64 CloudWorkerProgramId = 0x4200000000004E59ULL;
constexpr const char* ConfigDirectory = "sdmc:/config/NXSync";
constexpr const char* ConfigPath = "sdmc:/config/NXSync/sysmodule.ini";
constexpr const char* StatusPath = "sdmc:/config/NXSync/sysmodule.status";
constexpr const char* OverlayCatalogPath = "sdmc:/config/NXSync/overlay.catalog";
constexpr const char* NotificationDirectory =
    "sdmc:/config/ultrahand/notifications";
constexpr const char* QueueRoot = "sdmc:/config/NXSync/queue";
constexpr const char* CloudWorkerLockPath = "sdmc:/config/NXSync/cloud-worker.lock";
constexpr const char* BackupRequestRoot = "sdmc:/config/NXSync/backup-requests";
constexpr const char* PreflightRequestPath =
    "sdmc:/config/NXSync/preflight.request";
constexpr const char* PreflightStatusPath =
    "sdmc:/config/NXSync/preflight.status";
constexpr const char* LaunchRequestPath =
    "sdmc:/config/NXSync/launch.request";
constexpr const char* LaunchDecisionPath =
    "sdmc:/config/NXSync/launch.decision";
constexpr const char* LaunchWorkerResultPath =
    "sdmc:/config/NXSync/launch.worker-result";
constexpr const char* LaunchActionPath =
    "sdmc:/config/NXSync/launch.action";
constexpr const char* LocalResolutionPath =
    "sdmc:/config/NXSync/local-resolution.pending";
constexpr const char* OverlayOpenRequestPath =
    "sdmc:/config/NXSync/overlay-open.request";
constexpr const char* LaunchGateEnabledPath =
    "sdmc:/config/NXSync/launch-gate.enabled";
constexpr std::uint32_t BackupSettleDelaySeconds = 2;
constexpr std::uint32_t CloudWorkerReleaseDelaySeconds = 2;
constexpr std::uint32_t CloudWorkerRetryDelaySeconds = 300;
constexpr std::uint32_t CloudWorkerStartupGraceSeconds = 15;
constexpr std::uint64_t LaunchGatePollIntervalNs = 250'000'000ULL;
constexpr std::uint64_t LaunchResultNotificationDelayNs = 1'500'000'000ULL;
constexpr std::size_t InnerHeapSize = 0x18000;

Result gPglInitializationResult = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
Result gPmInfoInitializationResult = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
bool gPglInitialized = false;
bool gPmInfoInitialized = false;
bool gTimeInitialized = false;
nxsync::StorageEnvironment gStorageEnvironment =
    nxsync::StorageEnvironment::Unknown;
Result gStorageDetectionResult = 0;
const char* gStorageDetectionErrorPrefix = nullptr;
bool gEmummcOnly = true;
bool gAutomationsAllowed = false;
bool gBackupOnGameExit = true;
bool gPreflightEnabled = false;
u64 gProcessId = 0;

struct PublishedBackupState {
    std::string programId;
    std::string result;
    std::size_t created{0};
    std::size_t unchanged{0};
    std::string archivePath;
};

struct PublishedLaunchGateState {
    bool enabled{false};
    std::string state{"disabled"};
    std::string lastProgramId;
    std::uint64_t lastProcessId{0};
    std::string lastResult;
    std::uint64_t sequence{0};
};

PublishedLaunchGateState gLaunchGate;

void applyAutomationPolicy(const nxsync::SysmoduleConfig& config) {
    gEmummcOnly = config.emummcOnly;
    gBackupOnGameExit = config.backupOnGameExit;
    gPreflightEnabled = config.preflightEnabled;
    gAutomationsAllowed = nxsync::automationScopeAllows(
        config.emummcOnly, gStorageEnvironment);
}

bool ensureDirectories() {
    if (mkdir("sdmc:/config", 0777) != 0 && errno != EEXIST) return false;
    return mkdir(ConfigDirectory, 0777) == 0 || errno == EEXIST;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path);
    return static_cast<bool>(input);
}

void updateLaunchGateFlag(const bool enabled) {
    if (!enabled) {
        if (std::remove(LaunchGateEnabledPath) != 0 && errno != ENOENT) {
            // The next loop retries implicitly when the configuration changes.
        }
        return;
    }
    std::ofstream output(LaunchGateEnabledPath, std::ios::trunc);
    if (output) {
        output << "nxsync-automatic-launch-gate-v1\n";
        output.flush();
    }
}

bool writeStatusAtomic(const std::string& text, int& systemError) {
    return nxsync::writeTextFileAtomic(StatusPath, text, systemError);
}

nxsync::SysmoduleConfig readConfiguration(std::string& error) {
    nxsync::SysmoduleConfig config;
    if (nxsync::loadSysmoduleConfig(ConfigPath, config, error)) {
        return config;
    }
    if (fileExists(ConfigPath)) {
        config.enabled = false;
        return config;
    }
    int systemError = 0;
    if (!nxsync::writeSysmoduleConfig(ConfigPath, config, systemError)) {
        error = "Unable to create sysmodule.ini (errno "
            + std::to_string(systemError) + ")";
        return config;
    }
    error.clear();
    return config;
}

std::string formatProgramId(const u64 programId) {
    char value[17]{};
    std::snprintf(
        value,
        sizeof(value),
        "%016llX",
        static_cast<unsigned long long>(programId));
    return value;
}

std::string notificationGameName(const std::string& titleId) {
    nxsync::OverlayCatalog catalog;
    std::string error;
    if (nxsync::loadOverlayCatalog(OverlayCatalogPath, catalog, error)) {
        const auto found = std::find_if(
            catalog.entries.begin(), catalog.entries.end(),
            [&](const nxsync::OverlayCatalogEntry& entry) {
                return entry.titleId == titleId;
            });
        if (found != catalog.entries.end() && !found->titleName.empty()) {
            return found->titleName;
        }
    }
    return "Title " + titleId;
}

void notifyNxsync(
    const std::string& titleId,
    const std::string& message,
    const int priority = 20,
    const unsigned durationMs = 4500) {
    nxsync::UltrahandNotification notification;
    notification.title = "NXSync";
    notification.text = notificationGameName(titleId) + ": " + message;
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

std::string formatResult(const char* prefix, const Result result) {
    char value[96]{};
    std::snprintf(
        value,
        sizeof(value),
        "%s (0x%08X)",
        prefix,
        static_cast<unsigned>(result));
    return value;
}

bool backupProbeFailed(const nxsync::BackupRequestProbeSummary& probe) {
    return probe.systemError != 0 || probe.nativeResult != 0;
}

std::string describeBackupProbeFailure(
    const nxsync::BackupRequestProbeSummary& probe,
    const char* context) {
    const std::string stage = nxsync::backupRequestProbeStageName(
        probe.failureStage);
    if (probe.nativeResult != 0) {
        char result[11]{};
        std::snprintf(
            result,
            sizeof(result),
            "0x%08X",
            static_cast<unsigned>(probe.nativeResult));
        return std::string(context) + " stage=" + stage
            + " result=" + result;
    }
    return std::string(context) + " stage=" + stage
        + " errno=" + std::to_string(probe.systemError);
}

void copyLifecycleStatus(
    const nxsync::ApplicationLifecycleState& lifecycle,
    nxsync::SysmoduleStatus& status) {
    status.activeProgramId = lifecycle.activeProgramId;
    status.activeProcessId = lifecycle.activeProcessId;
    status.lastProgramId = lifecycle.lastProgramId;
    status.lastEvent = lifecycle.lastEvent;
    status.eventSequence = lifecycle.eventSequence;
}

bool publishStatus(
    const nxsync::ApplicationLifecycleState& lifecycle,
    const std::string& lastQueuedProgramId,
    const PublishedBackupState& backup,
    const bool enabled,
    const std::string& state,
    const std::string& lastError,
    std::string& previousStatus) {
    nxsync::SysmoduleStatus status;
    status.buildVersion = BuildVersion;
    status.processId = gProcessId;
    status.backupOnGameExit = gBackupOnGameExit;
    status.preflightEnabled = gPreflightEnabled;
    status.enabled = enabled;
    status.state = state;
    const std::string currentEnvironment = nxsync::storageEnvironmentKey(
        gStorageEnvironment);
    status.pendingOperations = nxsync::loadPendingCloudOperations(
        QueueRoot, currentEnvironment).size();
    const nxsync::BackupRequestProbeSummary backupProbe =
        nxsync::probePendingBackupRequests(
            BackupRequestRoot,
            currentEnvironment.c_str(),
            armTicksToNs(armGetSystemTick()),
            0);
    if (!lastError.empty()) {
        status.lastError = lastError;
    } else if (backupProbeFailed(backupProbe)) {
        status.lastError = describeBackupProbeFailure(
            backupProbe, "Backup request scan failed");
    } else if (backupProbe.invalidCount > 0) {
        status.lastError = "Invalid backup requests: "
            + std::to_string(backupProbe.invalidCount);
    } else if (gStorageEnvironment == nxsync::StorageEnvironment::Unknown
        && gStorageDetectionErrorPrefix != nullptr) {
        status.lastError = formatResult(
            gStorageDetectionErrorPrefix, gStorageDetectionResult);
    }
    copyLifecycleStatus(lifecycle, status);
    status.pendingBackupRequests = backupProbe.validCount;
    status.lastQueuedProgramId = lastQueuedProgramId;
    status.lastBackupProgramId = backup.programId;
    status.lastBackupResult = backup.result;
    status.lastBackupCreated = backup.created;
    status.lastBackupUnchanged = backup.unchanged;
    status.lastBackupArchivePath = backup.archivePath;
    status.backupStage.clear();
    status.backupCurrentPath.clear();
    status.backupFilesProcessed = 0;
    status.backupTotalFiles = 0;
    status.backupBytesProcessed = 0;
    status.backupTotalBytes = 0;
    status.launchGateEnabled = gLaunchGate.enabled;
    status.launchGateState = gLaunchGate.state;
    status.lastPreflightProgramId = gLaunchGate.lastProgramId;
    status.lastPreflightProcessId = gLaunchGate.lastProcessId;
    status.lastPreflightResult = gLaunchGate.lastResult;
    status.preflightSequence = gLaunchGate.sequence;
    status.storageEnvironment = nxsync::storageEnvironmentKey(
        gStorageEnvironment);
    status.automationScope = gEmummcOnly ? "emummc" : "all";
    status.automationsAllowed = gAutomationsAllowed;
    const std::string serialized = nxsync::serializeSysmoduleStatus(status);
    if (serialized == previousStatus) return true;
    int systemError = 0;
    if (!writeStatusAtomic(serialized, systemError)) return false;
    previousStatus = serialized;
    return true;
}

bool detectCurrentApplication(
    nxsync::ApplicationLifecycleState& lifecycle,
    const bool detectedAtStartup,
    std::string& error) {
    u64 processId = 0;
    const Result processResult = pglGetApplicationProcessId(&processId);
    if (R_FAILED(processResult) || processId == 0) return false;
    u64 programId = 0;
    const Result programResult = pminfoGetProgramId(&programId, processId);
    if (R_FAILED(programResult)) {
        error = formatResult("Application title ID unavailable", programResult);
        return false;
    }
    // The transient cloud worker uses the application resource group so it can
    // start while qlaunch owns the applet group. It is infrastructure, not a
    // game, and must never enter the save-backup lifecycle.
    if (programId == CloudWorkerProgramId) return false;
    error.clear();
    return nxsync::recordApplicationStart(
        lifecycle,
        processId,
        formatProgramId(programId),
        detectedAtStartup);
}

void queueBackupForLastTermination(
    const nxsync::ApplicationLifecycleState& lifecycle,
    std::string& lastQueuedProgramId,
    std::string& error) {
    // Re-read at the event boundary: disabling backups must not enqueue a new
    // request while the main loop is still waiting for its next config reload.
    nxsync::SysmoduleConfig currentConfig;
    if (!nxsync::loadSysmoduleConfig(ConfigPath, currentConfig, error)) return;
    if (!currentConfig.enabled || !currentConfig.backupOnGameExit
        || !nxsync::automationScopeAllows(currentConfig.emummcOnly, gStorageEnvironment)) {
        error.clear();
        return;
    }
    nxsync::PendingBackupRequest request;
    request.storageEnvironment = nxsync::storageEnvironmentKey(
        gStorageEnvironment);
    request.titleId = lifecycle.lastProgramId;
    request.triggerEvent = lifecycle.lastEvent;
    request.eventSequence = lifecycle.eventSequence;
    request.requestedMonotonicNs = armTicksToNs(armGetSystemTick());
    request.settleDelaySeconds = BackupSettleDelaySeconds;
    request.notBeforeMonotonicNs = request.requestedMonotonicNs
        + static_cast<std::uint64_t>(request.settleDelaySeconds)
            * 1'000'000'000ULL;
    int queueError = 0;
    if (nxsync::enqueueBackupRequest(
            BackupRequestRoot, request, queueError)) {
        lastQueuedProgramId = request.titleId;
        error.clear();
        notifyNxsync(
            request.titleId,
            "game closed; starting automatic backup immediately");
    } else {
        error = "Unable to queue the backup (errno "
            + std::to_string(queueError) + ")";
        notifyNxsync(request.titleId, error, 30, 6500);
    }
}

bool recordTerminationAndQueueBackup(
    nxsync::ApplicationLifecycleState& lifecycle,
    const u64 processId,
    const bool crashed,
    std::string& lastQueuedProgramId,
    std::string& error) {
    if (!nxsync::recordApplicationTermination(
            lifecycle, processId, crashed)) {
        return false;
    }
    queueBackupForLastTermination(
        lifecycle, lastQueuedProgramId, error);
    return true;
}

Result launchCloudWorker(u64& processId) {
    NcmProgramLocation location{};
    location.program_id = CloudWorkerProgramId;
    location.storageID = NcmStorageId_None;
    return pglLaunchProgram(
        &processId,
        &location,
        PmLaunchFlag_SignalOnExit,
        PglLaunchFlag_None);
}

bool applicationSlotIsFree() {
    u64 processId = 0;
    const Result result = pglGetApplicationProcessId(&processId);
    return R_FAILED(result) || processId == 0;
}

bool allowLaunch(
    const nxsync::LaunchRequest& request,
    const std::string& message,
    std::string& error,
    const std::string& requestedAction = "allow") {
    nxsync::LaunchDecision decision;
    decision.sequence = request.sequence;
    struct stat guardInfo{};
    const bool guarded = stat(nxsync::LaunchRestoreGuardPath, &guardInfo) == 0
        || stat((std::string(nxsync::LaunchRestoreGuardPath) + ".bak").c_str(), &guardInfo) == 0;
    decision.action = guarded ? "abort" : requestedAction;
    decision.message = message;
    int systemError = 0;
    if (!nxsync::writeLaunchDecisionAtomic(
            LaunchDecisionPath, decision, systemError)) {
        error = "Unable to authorize launch (errno "
            + std::to_string(systemError) + ")";
        return false;
    }
    gLaunchGate.state = decision.action == "allow" ? "allowed" : "blocked";
    gLaunchGate.lastResult = message;
    error.clear();
    return true;
}

bool queueAutomaticPreflight(
    const nxsync::LaunchRequest& request,
    std::string& error) {
    nxsync::PreflightRequest preflight;
    preflight.sequence = request.sequence;
    preflight.titleId = request.titleId;
    preflight.automaticLaunch = true;
    nxsync::OverlayCatalog catalog;
    std::string catalogError;
    if (nxsync::loadOverlayCatalog(
            OverlayCatalogPath, catalog, catalogError)) {
        nxsync::findUniqueProfileUidForTitle(
            catalog, request.titleId, preflight.profileUid);
    }
    int systemError = 0;
    if (!nxsync::writePreflightRequestAtomic(
            PreflightRequestPath, preflight, systemError)) {
        error = "Unable to queue the automatic check (errno "
            + std::to_string(systemError) + ")";
        return false;
    }
    gLaunchGate.state = "checking-cloud";
    gLaunchGate.lastResult = "Automatic cloud check queued";
    error.clear();
    return true;
}

bool recordSelectedLocalResolution(
    const nxsync::LaunchRequest& request,
    const nxsync::LaunchAction& action,
    std::string& error) {
    nxsync::PreflightStatus status;
    std::string statusError;
    if (!nxsync::loadPreflightStatus(
            PreflightStatusPath, status, statusError)
        || status.sequence != request.sequence
        || status.titleId != request.titleId
        || (status.outcome != "conflict"
            && status.outcome != "cloud-update-available")
        || status.profileUid.empty()) {
        error.clear();
        return false;
    }
    if (status.localRevisionId.empty() || status.localPayloadSha256.empty()) {
        error = "Local conflict revision unavailable";
        return false;
    }

    std::vector<std::string> cloudParents;
    if (!action.selectedRevisionId.empty()) {
        const auto selected = std::find_if(
            status.candidates.begin(), status.candidates.end(),
            [&](const nxsync::PreflightCandidate& candidate) {
                return candidate.revisionId == action.selectedRevisionId;
            });
        if (selected == status.candidates.end()) {
            error = "Selected cloud branch unavailable";
            return false;
        }
        cloudParents.push_back(selected->revisionId);
    } else {
        for (const nxsync::PreflightCandidate& candidate : status.candidates) {
            if (std::find(
                    cloudParents.begin(),
                    cloudParents.end(),
                    candidate.revisionId) == cloudParents.end()) {
                cloudParents.push_back(candidate.revisionId);
            }
        }
    }
    if (cloudParents.empty()
        || cloudParents.size() + 1 > nxsync::MaximumRevisionParents) {
        error = "The number of cloud branches cannot be resolved automatically";
        return false;
    }

    nxsync::PendingLocalResolution resolution;
    resolution.sequence = request.sequence;
    resolution.storageEnvironment =
        nxsync::storageEnvironmentKey(gStorageEnvironment);
    resolution.titleId = request.titleId;
    resolution.profileUid = status.profileUid;
    resolution.parentRevisionId = status.localRevisionId;
    resolution.parentPayloadSha256 = status.localPayloadSha256;
    resolution.parentRevisionIds = {status.localRevisionId};
    resolution.parentRevisionIds.insert(
        resolution.parentRevisionIds.end(),
        cloudParents.begin(),
        cloudParents.end());
    int systemError = 0;
    if (!nxsync::writePendingLocalResolutionAtomic(
            LocalResolutionPath, resolution, systemError)) {
        error = "Unable to record the local resolution (errno "
            + std::to_string(systemError) + ")";
        return false;
    }
    error.clear();
    return true;
}

bool createCloudWorkerLock() {
    std::ofstream output(CloudWorkerLockPath, std::ios::trunc);
    if (!output) return false;
    output << "launching\n";
    output.flush();
    return output.good();
}

void removeCloudWorkerLock() {
    if (std::remove(CloudWorkerLockPath) != 0 && errno != ENOENT) {
        // The next launch attempt will overwrite the stale lock.
    }
}

bool clearStaleLaunchSession(std::string& error) {
    const char* const paths[] = {
        LaunchRequestPath,
        LaunchDecisionPath,
        LaunchActionPath,
        nxsync::LaunchRestoreClaimPath,
        nxsync::LaunchRestoreGrantPath,
        nxsync::LaunchPhasePath,
        PreflightRequestPath,
        PreflightStatusPath,
        OverlayOpenRequestPath,
    };
    for (const char* path : paths) {
        int cleanupError = 0;
        if (!nxsync::removeTextFileRecoverable(path, cleanupError)) {
            error = "Unable to clean up the previous launch session (errno "
                + std::to_string(errno) + ")";
            return false;
        }
    }
    error.clear();
    return true;
}

bool finishLaunchSessionForProcess(
    const std::uint64_t processId,
    const bool gateEnabled,
    std::string& error) {
    if (processId == 0 || gLaunchGate.lastProcessId != processId) return false;

    const std::string completedTitleId = gLaunchGate.lastProgramId;
    if (!clearStaleLaunchSession(error)) return false;

    gLaunchGate.state = gateEnabled ? "automatic-ready" : "disabled";
    gLaunchGate.lastProgramId = completedTitleId;
    gLaunchGate.lastProcessId = 0;
    gLaunchGate.lastResult = gateEnabled
        ? "Previous session completed; ready for a new check"
        : std::string();
    gLaunchGate.sequence = 0;
    return true;
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
    if (R_FAILED(result)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    }
    result = setsysInitialize();
    if (R_SUCCEEDED(result)) {
        SetSysFirmwareVersion firmware{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware))) {
            hosversionSet(MAKEHOSVERSION(
                firmware.major,
                firmware.minor,
                firmware.micro));
            gStorageEnvironment = nxsync::parseAtmosphereStorageEnvironment(
                std::string(
                    firmware.display_version,
                    strnlen(
                        firmware.display_version,
                        sizeof(firmware.display_version))));
        }
        setsysExit();
    }
    const Result splResult = splInitialize();
    if (R_SUCCEEDED(splResult)) {
        u64 emummcType = 0;
        const Result configResult = splGetConfig(
            static_cast<SplConfigItem>(65007), &emummcType);
        if (R_SUCCEEDED(configResult)) {
            gStorageEnvironment =
                nxsync::classifyAtmosphereEmummcType(emummcType);
            gStorageDetectionResult = 0;
            gStorageDetectionErrorPrefix = nullptr;
        } else {
            gStorageDetectionResult = configResult;
            gStorageDetectionErrorPrefix =
                "Unable to read ExosphereEmummcType";
        }
        splExit();
    } else {
        gStorageDetectionResult = splResult;
        gStorageDetectionErrorPrefix = "SPL service unavailable";
    }
    result = fsInitialize();
    if (R_FAILED(result)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    }
    result = fsdevMountSdmc();
    if (R_FAILED(result)) {
        diagAbortWithResult(result);
    }
}

void __appExit(void) {
    if (gTimeInitialized) timeExit();
    if (gPmInfoInitialized) pminfoExit();
    if (gPglInitialized) pglExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

} // extern "C"

int main(int, char**) {
    svcGetProcessId(&gProcessId, CUR_PROCESS_HANDLE);
    ensureDirectories();
    // dmnt reads this durable flag before the observer is necessarily ready.
    // Disable the gate first, then discard protocol records from the previous
    // boot: no suspended launch can legitimately survive a full reboot.
    updateLaunchGateFlag(false);
    std::string previousStatus;
    nxsync::ApplicationLifecycleState lifecycle;
    PublishedBackupState backupState;
    u64 cloudWorkerProcessId = 0;
    bool cloudWorkerCrashPending = false;
    bool cloudWorkerExitObserved = false;
    std::uint64_t workerLaunchNotBeforeNs = 0;
    std::uint64_t nextCloudWorkerAttemptNs = armTicksToNs(armGetSystemTick())
        + static_cast<std::uint64_t>(CloudWorkerStartupGraceSeconds)
            * 1'000'000'000ULL;
    std::string lastQueuedProgramId;
    std::string observerError;
    std::uint64_t pendingLaunchNotificationSequence = 0;
    std::uint64_t launchNotificationNotBeforeNs = 0;
    std::uint64_t notifiedLaunchSequence = 0;
    clearStaleLaunchSession(observerError);
    const std::string initialEnvironment =
        nxsync::storageEnvironmentKey(gStorageEnvironment);
    const nxsync::BackupRequestProbeSummary initialBackupProbe =
        nxsync::probePendingBackupRequests(
            BackupRequestRoot,
            initialEnvironment.c_str(),
            armTicksToNs(armGetSystemTick()),
            0);
    if (initialBackupProbe.latestTitleId[0] != '\0') {
        lastQueuedProgramId = initialBackupProbe.latestTitleId.data();
    }
    if (backupProbeFailed(initialBackupProbe)) {
        observerError = describeBackupProbeFailure(
            initialBackupProbe,
            "Initial backup request scan failed");
    } else if (initialBackupProbe.invalidCount > 0) {
        observerError = "Invalid initial backup requests: "
            + std::to_string(initialBackupProbe.invalidCount);
    }
    std::string configError;
    nxsync::SysmoduleConfig config = readConfiguration(configError);
    applyAutomationPolicy(config);
    if (!configError.empty()) {
        updateLaunchGateFlag(false);
        publishStatus(
            lifecycle, lastQueuedProgramId, backupState, false,
            "configuration-error", configError, previousStatus);
        return 0;
    }
    if (!config.enabled) {
        updateLaunchGateFlag(false);
        publishStatus(
            lifecycle, lastQueuedProgramId, backupState,
            false, "disabled", {}, previousStatus);
        return 0;
    }

    // With the default emuMMC-only policy, do not merely suppress backup and
    // preflight work on sysMMC: leave no resident PGL observer behind. Keeping
    // the process alive still consumes system-pool resources and changes the
    // timing of application/HID services even though automations are blocked.
    // A scope change made while this process is absent takes effect on reboot.
    if (!nxsync::sysmoduleObserverShouldRun(
            config.enabled, config.emummcOnly, gStorageEnvironment)) {
        updateLaunchGateFlag(false);
        gLaunchGate.enabled = false;
        gLaunchGate.state = "scope-blocked";
        gLaunchGate.lastResult =
            "Sysmodule is not resident in this environment; reboot after changing scope";
        publishStatus(
            lifecycle,
            lastQueuedProgramId,
            backupState,
            true,
            "scope-blocked",
            {},
            previousStatus);
        return 0;
    }

    // Observer-only services are initialized only after the NAND policy has
    // explicitly authorized this process to remain resident.
    gPglInitializationResult = pglInitialize();
    gPglInitialized = R_SUCCEEDED(gPglInitializationResult);
    gPmInfoInitializationResult = pminfoInitialize();
    gPmInfoInitialized = R_SUCCEEDED(gPmInfoInitializationResult);
    if (!gPglInitialized || !gPmInfoInitialized) {
        updateLaunchGateFlag(false);
        const std::string error = !gPglInitialized
            ? formatResult("PGL service unavailable", gPglInitializationResult)
            : formatResult("pm:info service unavailable", gPmInfoInitializationResult);
        publishStatus(
            lifecycle, lastQueuedProgramId, backupState,
            true, "observer-error", error, previousStatus);
        return 0;
    }
    removeCloudWorkerLock();
    { int ignored = 0; nxsync::removeTextFileRecoverable(LaunchWorkerResultPath, ignored); }
    gTimeInitialized = R_SUCCEEDED(timeInitialize());
    const nxsync::OverlayDependencyStatus overlayDependencies =
        nxsync::detectOverlayDependencies();
    const bool effectivePreflightEnabled = config.preflightEnabled
        && gAutomationsAllowed
        && overlayDependencies.ready();
    gLaunchGate.enabled = effectivePreflightEnabled;
    updateLaunchGateFlag(effectivePreflightEnabled);
    gLaunchGate.state = !gAutomationsAllowed
        ? "scope-blocked"
        : (!config.preflightEnabled
            ? "disabled"
            : (!overlayDependencies.ready()
                ? "overlay-dependencies-missing"
                : "automatic-ready"));
    gLaunchGate.lastResult = !gAutomationsAllowed
        ? "Automation disabled by NAND scope"
        : (!config.preflightEnabled
            ? std::string()
            : (!overlayDependencies.ready()
                ? "Automatic preflight blocked; missing: "
                    + nxsync::missingOverlayDependencySummary(
                        overlayDependencies)
                : "Automatic gate ready; fail-open timeout handled by dmnt"));

    PglEventObserver observer{};
    Result result = pglGetEventObserver(&observer);
    if (R_FAILED(result)) {
        publishStatus(
            lifecycle,
            lastQueuedProgramId,
            backupState,
            true,
            "observer-error",
            formatResult("PGL observer unavailable", result),
            previousStatus);
        return 0;
    }
    Event processEvent{};
    result = pglEventObserverGetProcessEvent(&observer, &processEvent);
    if (R_FAILED(result)) {
        pglEventObserverClose(&observer);
        publishStatus(
            lifecycle,
            lastQueuedProgramId,
            backupState,
            true,
            "observer-error",
            formatResult("PGL event unavailable", result),
            previousStatus);
        return 0;
    }

    detectCurrentApplication(lifecycle, true, observerError);
    publishStatus(
        lifecycle, lastQueuedProgramId, backupState,
        true,
        gAutomationsAllowed ? "observer-ready" : "scope-blocked",
        observerError,
        previousStatus);

    bool previousOverlayDependenciesReady = overlayDependencies.ready();
    while (true) {
        const u64 maximumTimeout = static_cast<u64>(config.pollIntervalSeconds)
            * 1'000'000'000ULL;
        u64 timeout = gLaunchGate.enabled
            ? std::min<u64>(maximumTimeout, LaunchGatePollIntervalNs)
            : maximumTimeout;
        if (gAutomationsAllowed
            && lifecycle.activeProcessId == 0
            && cloudWorkerProcessId == 0) {
            const std::uint64_t nowNs = armTicksToNs(armGetSystemTick());
            if (nxsync::cloudWorkerReleaseDelayElapsed(
                    nowNs, workerLaunchNotBeforeNs)) {
                const std::string environment =
                    nxsync::storageEnvironmentKey(gStorageEnvironment);
                const nxsync::BackupRequestProbeSummary backupProbe =
                    nxsync::probePendingBackupRequests(
                        BackupRequestRoot,
                        environment.c_str(),
                        nowNs,
                        timeout);
                timeout = backupProbe.nextWaitNs;
                if (backupProbeFailed(backupProbe)) {
                    observerError = describeBackupProbeFailure(
                        backupProbe, "Backup request scan failed");
                } else if (backupProbe.invalidCount > 0) {
                    observerError = "Invalid backup requests: "
                        + std::to_string(backupProbe.invalidCount);
                }
            } else {
                timeout = std::min<u64>(
                    timeout, workerLaunchNotBeforeNs - nowNs);
            }
        }
        const Result waitResult = eventWait(&processEvent, timeout);
        if (R_SUCCEEDED(waitResult)) {
            PmProcessEventInfo eventInfo{};
            const Result infoResult = pglEventObserverGetProcessEventInfo(
                &observer,
                &eventInfo);
            if (R_SUCCEEDED(infoResult)) {
                if (eventInfo.event == PmProcessEvent_Start) {
                    u64 applicationProcessId = 0;
                    if (eventInfo.process_id != cloudWorkerProcessId
                        && R_SUCCEEDED(pglGetApplicationProcessId(
                        &applicationProcessId))
                        && applicationProcessId == eventInfo.process_id) {
                        detectCurrentApplication(lifecycle, false, observerError);
                    }
                } else if (eventInfo.event == PmProcessEvent_Exit
                    || eventInfo.event == PmProcessEvent_Crash) {
                    const bool cloudWorkerEvent = cloudWorkerProcessId != 0
                        && eventInfo.process_id == cloudWorkerProcessId;
                    if (cloudWorkerEvent) {
                        if (eventInfo.event == PmProcessEvent_Crash) {
                            // Crash is an exception notification, not the final
                            // process exit. Keep the PID and terminate it so the
                            // Application resource slot cannot remain occupied.
                            cloudWorkerCrashPending = true;
                            const Result terminateResult = pglTerminateProcess(
                                cloudWorkerProcessId);
                            observerError = R_SUCCEEDED(terminateResult)
                                ? "Cloud worker crashed: stopping and releasing resources"
                                : formatResult(
                                    "Unable to stop the crashed cloud worker",
                                    terminateResult);
                        } else {
                            const bool crashed = cloudWorkerCrashPending;
                            const std::uint64_t workerExitNowNs =
                                armTicksToNs(armGetSystemTick());
                            cloudWorkerProcessId = 0;
                            cloudWorkerExitObserved = true;
                            workerLaunchNotBeforeNs = workerExitNowNs
                                + static_cast<std::uint64_t>(
                                    CloudWorkerReleaseDelaySeconds)
                                    * 1'000'000'000ULL;
                            removeCloudWorkerLock();
                            const std::size_t pendingOperations =
                                nxsync::loadPendingCloudOperations(
                                    QueueRoot,
                                    nxsync::storageEnvironmentKey(
                                        gStorageEnvironment)).size();
                            nextCloudWorkerAttemptNs =
                                nxsync::cloudWorkerNeedsRetry(
                                    crashed,
                                    pendingOperations)
                                ? workerExitNowNs
                                    + static_cast<std::uint64_t>(
                                        CloudWorkerRetryDelaySeconds)
                                        * 1'000'000'000ULL
                                : 0;
                            observerError = crashed
                                ? "Cloud worker stopped after a crash"
                                : (pendingOperations > 0
                                    ? "Cloud worker exited with pending operations; retry deferred"
                                    : std::string());
                            cloudWorkerCrashPending = false;
                        }
                    }
                    if (!cloudWorkerEvent) {
                        if (finishLaunchSessionForProcess(
                                eventInfo.process_id,
                                gLaunchGate.enabled,
                                observerError)) {
                            pendingLaunchNotificationSequence = 0;
                            launchNotificationNotBeforeNs = 0;
                            notifiedLaunchSequence = 0;
                        }
                        recordTerminationAndQueueBackup(
                            lifecycle,
                            eventInfo.process_id,
                            eventInfo.event == PmProcessEvent_Crash,
                            lastQueuedProgramId,
                            observerError);
                    }
                }
            } else {
                observerError = formatResult("Unable to read PGL event", infoResult);
            }
        } else {
            // PGL notifications can race with the transient restore worker. On
            // every periodic wake-up, reconcile both missed starts and missed
            // exits. A failed current-application query is accepted as an exit
            // only after pm:info confirms that the tracked PID no longer exists.
            u64 observedProcessId = 0;
            const Result observedResult = pglGetApplicationProcessId(
                &observedProcessId);
            bool trackedProcessStillExists = false;
            if (lifecycle.activeProcessId != 0
                && (R_FAILED(observedResult)
                    || observedProcessId != lifecycle.activeProcessId)) {
                const u64 trackedProcessId = lifecycle.activeProcessId;
                u64 trackedProgramId = 0;
                trackedProcessStillExists = R_SUCCEEDED(pminfoGetProgramId(
                    &trackedProgramId, lifecycle.activeProcessId));
                if (nxsync::reconcileMissingApplication(
                        lifecycle,
                        R_SUCCEEDED(observedResult) ? observedProcessId : 0,
                        trackedProcessStillExists)) {
                    queueBackupForLastTermination(
                        lifecycle, lastQueuedProgramId, observerError);
                    if (finishLaunchSessionForProcess(
                            trackedProcessId,
                            gLaunchGate.enabled,
                            observerError)) {
                        pendingLaunchNotificationSequence = 0;
                        launchNotificationNotBeforeNs = 0;
                        notifiedLaunchSequence = 0;
                    }
                }
            }
            if (R_SUCCEEDED(observedResult)
                && observedProcessId != 0
                && observedProcessId != cloudWorkerProcessId
                && lifecycle.activeProcessId == 0) {
                std::string detectionError;
                detectCurrentApplication(lifecycle, false, detectionError);
                if (!detectionError.empty()) observerError = detectionError;
            }
        }

        // PGL exit notifications are not guaranteed to be observed while an
        // application is transitioning back to qlaunch. Reconcile the worker
        // PID through pm:info so a missed Exit event cannot block all later
        // backup and upload requests indefinitely.
        if (cloudWorkerProcessId != 0) {
            u64 observedWorkerProgramId = 0;
            const Result workerInfoResult = pminfoGetProgramId(
                &observedWorkerProgramId, cloudWorkerProcessId);
            if (nxsync::cloudWorkerPidIsStale(
                    R_SUCCEEDED(workerInfoResult),
                    observedWorkerProgramId == CloudWorkerProgramId)) {
                const std::uint64_t workerExitNowNs =
                    armTicksToNs(armGetSystemTick());
                cloudWorkerProcessId = 0;
                cloudWorkerCrashPending = false;
                cloudWorkerExitObserved = true;
                workerLaunchNotBeforeNs = workerExitNowNs
                    + static_cast<std::uint64_t>(
                        CloudWorkerReleaseDelaySeconds)
                        * 1'000'000'000ULL;
                removeCloudWorkerLock();
            }
        }

        const bool previousAutomationsAllowed = gAutomationsAllowed;
        const bool previousPreflightEnabled = config.preflightEnabled;
        config = readConfiguration(configError);
        applyAutomationPolicy(config);
        if (!configError.empty()) {
            updateLaunchGateFlag(false);
            publishStatus(
                lifecycle,
                lastQueuedProgramId,
                backupState,
                false,
                "configuration-error",
                configError,
                previousStatus);
            break;
        }
        if (!config.enabled) {
            updateLaunchGateFlag(false);
            publishStatus(
                lifecycle, lastQueuedProgramId, backupState,
                false, "disabled", {}, previousStatus);
            break;
        }
        const nxsync::OverlayDependencyStatus currentOverlayDependencies =
            nxsync::detectOverlayDependencies();
        const bool effectiveGateEnabled = config.preflightEnabled
            && gAutomationsAllowed
            && currentOverlayDependencies.ready();
        if (effectiveGateEnabled != gLaunchGate.enabled
            || previousAutomationsAllowed != gAutomationsAllowed
            || previousPreflightEnabled != config.preflightEnabled
            || previousOverlayDependenciesReady
                != currentOverlayDependencies.ready()) {
            if (gLaunchGate.enabled && !effectiveGateEnabled) {
                nxsync::LaunchRequest interruptedLaunch;
                std::string interruptedError;
                if (nxsync::loadLaunchRequest(
                        LaunchRequestPath,
                        interruptedLaunch,
                        interruptedError)) {
                    nxsync::LaunchDecision existingDecision;
                    std::string decisionError;
                    if (!nxsync::loadLaunchDecision(
                            LaunchDecisionPath,
                            existingDecision,
                            decisionError)
                        || existingDecision.sequence
                            != interruptedLaunch.sequence) {
                        const std::string interruption = !gAutomationsAllowed
                            ? "Automation scope changed: continuing with the local save"
                            : (!config.preflightEnabled
                                ? "Automatic preflight disabled: continuing with the local save"
                                : "Overlay dependencies unavailable: continuing with the local save");
                        allowLaunch(
                            interruptedLaunch,
                            interruption,
                            observerError);
                    }
                }
            }
            gLaunchGate.enabled = effectiveGateEnabled;
            updateLaunchGateFlag(effectiveGateEnabled);
            if (!gAutomationsAllowed) {
                gLaunchGate.state = "scope-blocked";
                gLaunchGate.lastResult =
                    "Automation disabled by NAND scope";
            } else if (!config.preflightEnabled) {
                gLaunchGate.state = "disabled";
                gLaunchGate.lastResult = "disabled-by-configuration";
            } else if (!currentOverlayDependencies.ready()) {
                gLaunchGate.state = "overlay-dependencies-missing";
                gLaunchGate.lastResult =
                    "Automatic preflight blocked; missing: "
                    + nxsync::missingOverlayDependencySummary(
                        currentOverlayDependencies);
            } else {
                gLaunchGate.state = "automatic-ready";
                gLaunchGate.lastResult =
                    "Automatic gate ready; fail-open timeout handled by dmnt";
            }
        }
        previousOverlayDependenciesReady =
            currentOverlayDependencies.ready();

        nxsync::LaunchRequest automaticLaunch;
        std::string launchRequestError;
        const bool hasAutomaticLaunch = effectiveGateEnabled
            && nxsync::loadLaunchRequest(
                LaunchRequestPath, automaticLaunch, launchRequestError);
        nxsync::LaunchAction launchAction;
        std::string launchActionError;
        bool hasLaunchAction = hasAutomaticLaunch
            && nxsync::loadLaunchAction(
                LaunchActionPath, launchAction, launchActionError)
            && launchAction.sequence == automaticLaunch.sequence;

        if (hasAutomaticLaunch && automaticLaunch.sequence != gLaunchGate.sequence) {
            cloudWorkerExitObserved = false;
            gLaunchGate.sequence = automaticLaunch.sequence;
            gLaunchGate.lastProgramId = automaticLaunch.titleId;
            gLaunchGate.lastProcessId = automaticLaunch.processId;
            notifyNxsync(
                automaticLaunch.titleId,
                "launch requested; checking cloud saves");
            { int ignored = 0; nxsync::removeTextFileRecoverable(LaunchDecisionPath, ignored); }
            { int ignored = 0; nxsync::removeTextFileRecoverable(LaunchWorkerResultPath, ignored); }
            { int ignored = 0; nxsync::removeTextFileRecoverable(LaunchActionPath, ignored); }
            std::remove(OverlayOpenRequestPath);
            { int ignored = 0; nxsync::removeTextFileRecoverable(PreflightStatusPath, ignored); }
            if (!queueAutomaticPreflight(automaticLaunch, observerError)) {
                allowLaunch(
                    automaticLaunch,
                    "Unable to queue check: continuing with the local save",
                    observerError);
            }
            hasLaunchAction = false;
        }

        if (hasAutomaticLaunch && automaticLaunch.sequence == gLaunchGate.sequence) {
            nxsync::LaunchDecision existingDecision;
            std::string decisionError;
            nxsync::LaunchDecision workerResult;
            std::string workerResultError;
            const bool hasWorkerResult = nxsync::loadLaunchDecision(
                    LaunchWorkerResultPath, workerResult, workerResultError)
                && workerResult.sequence == automaticLaunch.sequence;
            if (nxsync::loadLaunchDecision(
                    LaunchDecisionPath, existingDecision, decisionError)
                && existingDecision.sequence == automaticLaunch.sequence) {
                gLaunchGate.state = existingDecision.action == "allow" ? "allowed" : "blocked";
                gLaunchGate.lastResult = existingDecision.message;
            } else if (hasWorkerResult) {
                if (nxsync::launchReleaseDisposition(
                        true, cloudWorkerProcessId)
                    == nxsync::LaunchReleaseDisposition::WaitingForWorkerExit) {
                    gLaunchGate.state = "waiting-worker-exit";
                    gLaunchGate.lastResult =
                        "Restore completed; waiting for the worker to release memory";
                } else {
                    if (allowLaunch(
                            automaticLaunch,
                            workerResult.message,
                            observerError,
                            workerResult.action)) {
                        { int ignored = 0; nxsync::removeTextFileRecoverable(LaunchWorkerResultPath, ignored); }
                    }
                }
            } else if (hasLaunchAction && launchAction.action == "use-local") {
                if (nxsync::launchReleaseDisposition(
                        true, cloudWorkerProcessId)
                    == nxsync::LaunchReleaseDisposition::WaitingForWorkerExit) {
                    gLaunchGate.state = "waiting-worker-exit";
                    gLaunchGate.lastResult =
                        "Local choice recorded; waiting for the worker to exit";
                } else {
                    std::string localChoiceMessage =
                        "User choice: continue with the local save";
                    std::string resolutionError;
                    if (recordSelectedLocalResolution(
                            automaticLaunch,
                            launchAction,
                            resolutionError)) {
                        localChoiceMessage +=
                            "; logical merge queued for the next backup";
                    } else if (!resolutionError.empty()) {
                        localChoiceMessage += "; " + resolutionError;
                    }
                    allowLaunch(
                        automaticLaunch,
                        localChoiceMessage,
                        observerError);
                    int actionError = 0;
                    nxsync::completeLaunchAction(
                        LaunchActionPath, automaticLaunch.sequence, actionError);
                }
            } else if (hasLaunchAction && launchAction.action == "restore-cloud") {
                if (gLaunchGate.state != "restoring-cloud") {
                    int phaseError = 0;
                    if (!nxsync::writeLaunchPhaseAtomic(nxsync::LaunchPhasePath,
                            automaticLaunch.sequence, "download", phaseError)) {
                        allowLaunch(automaticLaunch,
                            "Unable to reserve download time: continuing with the local save", observerError);
                        hasLaunchAction = false;
                        continue;
                    }
                    cloudWorkerExitObserved = false;
                    notifyNxsync(
                        automaticLaunch.titleId,
                        "downloading and restoring the cloud save");
                }
                gLaunchGate.state = "restoring-cloud";
                gLaunchGate.lastResult = "Cloud download and restore in progress";
            } else {
                nxsync::PreflightStatus automaticStatus;
                std::string automaticStatusError;
                if (nxsync::loadPreflightStatus(
                        PreflightStatusPath,
                        automaticStatus,
                        automaticStatusError)
                    && automaticStatus.sequence == automaticLaunch.sequence) {
                    if (automaticStatus.state == "error") {
                        const std::string detail = automaticStatus.message.empty()
                            ? std::string("unspecified worker error")
                            : automaticStatus.message;
                        const std::string message = "Cloud check failed: "
                            + detail + "; continuing with the local save";
                        if (nxsync::launchReleaseDisposition(
                                true, cloudWorkerProcessId)
                            == nxsync::LaunchReleaseDisposition::WaitingForWorkerExit) {
                            gLaunchGate.state = "waiting-worker-exit";
                            gLaunchGate.lastResult =
                                "Check completed; waiting for the worker to release memory";
                        } else {
                            allowLaunch(
                                automaticLaunch,
                                message,
                                observerError);
                        }
                    } else if (automaticStatus.state == "completed") {
                        if (automaticStatus.outcome == "cloud-update-available"
                            || automaticStatus.outcome == "conflict") {
                            if (nxsync::launchReleaseDisposition(
                                    true, cloudWorkerProcessId)
                                == nxsync::LaunchReleaseDisposition::WaitingForWorkerExit) {
                                gLaunchGate.state = "waiting-worker-exit";
                                gLaunchGate.lastResult =
                                    "Check completed; waiting for the worker to release memory";
                            } else if (gLaunchGate.state != "waiting-choice") {
                                int overlayError = 0;
                                if (nxsync::writeLaunchPhaseAtomic(nxsync::LaunchPhasePath,
                                        automaticLaunch.sequence, "choice", overlayError)
                                    && nxsync::requestLaunchOverlay(
                                        OverlayOpenRequestPath, overlayError)) {
                                    gLaunchGate.state = "waiting-choice";
                                    gLaunchGate.lastResult = automaticStatus.message;
                                    notifyNxsync(
                                        automaticLaunch.titleId,
                                        "newer or conflicting cloud backup; choose from the overlay",
                                        25,
                                        6500);
                                } else {
                                    allowLaunch(
                                        automaticLaunch,
                                        "Overlay unavailable: continuing with the local save",
                                        observerError);
                                }
                            }
                        } else {
                            if (nxsync::launchReleaseDisposition(
                                    true, cloudWorkerProcessId)
                                == nxsync::LaunchReleaseDisposition::WaitingForWorkerExit) {
                                gLaunchGate.state = "waiting-worker-exit";
                                gLaunchGate.lastResult =
                                    "Check completed; waiting for the worker to release memory";
                            } else {
                                allowLaunch(
                                    automaticLaunch,
                                    automaticStatus.message.empty()
                                        ? "Local save can be used"
                                        : automaticStatus.message,
                                    observerError);
                            }
                        }
                    }
                }
            }
        }
        if (hasAutomaticLaunch
            && cloudWorkerProcessId == 0
            && (gLaunchGate.state == "checking-cloud"
                || gLaunchGate.state == "restoring-cloud"
                || gLaunchGate.state == "waiting-worker-exit")
            && cloudWorkerExitObserved) {
            const std::string failure = observerError.empty()
                ? std::string("Worker exited without a valid result")
                : observerError;
            allowLaunch(
                automaticLaunch,
                failure + ": continuing with the local save",
                observerError);
        }
        if (gLaunchGate.state == "allowed"
            && gLaunchGate.sequence != 0
            && gLaunchGate.sequence != notifiedLaunchSequence
            && pendingLaunchNotificationSequence != gLaunchGate.sequence) {
            pendingLaunchNotificationSequence = gLaunchGate.sequence;
            launchNotificationNotBeforeNs = armTicksToNs(armGetSystemTick())
                + LaunchResultNotificationDelayNs;
        }
        if (pendingLaunchNotificationSequence != 0
            && pendingLaunchNotificationSequence == gLaunchGate.sequence
            && armTicksToNs(armGetSystemTick())
                >= launchNotificationNotBeforeNs
            && lifecycle.activeProgramId == gLaunchGate.lastProgramId) {
            notifyNxsync(
                gLaunchGate.lastProgramId,
                "cloud check completed: "
                    + (gLaunchGate.lastResult.empty()
                        ? std::string("launch authorized")
                        : gLaunchGate.lastResult),
                25,
                6000);
            notifiedLaunchSequence = pendingLaunchNotificationSequence;
            pendingLaunchNotificationSequence = 0;
            launchNotificationNotBeforeNs = 0;
        }
        nxsync::PreflightRequest pendingPreflight;
        std::string preflightError;
        const bool hasPendingPreflight = effectiveGateEnabled
            && nxsync::loadPreflightRequest(
                PreflightRequestPath,
                pendingPreflight,
                preflightError);
        const bool hasPendingLaunchRestore = hasAutomaticLaunch
            && gLaunchGate.state == "restoring-cloud"
            && hasLaunchAction
            && launchAction.action == "restore-cloud";
        const std::uint64_t workerCheckNowNs = armTicksToNs(armGetSystemTick());
        const std::string workerCheckEnvironment =
            nxsync::storageEnvironmentKey(gStorageEnvironment);
        const nxsync::BackupRequestProbeSummary pendingBackupProbe =
            nxsync::probePendingBackupRequests(
                BackupRequestRoot,
                workerCheckEnvironment.c_str(),
                workerCheckNowNs,
                maximumTimeout);
        const bool hasReadyBackupRequest =
            pendingBackupProbe.hasReadyRequest;
        if (backupProbeFailed(pendingBackupProbe)) {
            observerError = describeBackupProbeFailure(
                pendingBackupProbe, "Backup request scan failed");
        } else if (pendingBackupProbe.invalidCount > 0) {
            observerError = "Invalid backup requests: "
                + std::to_string(pendingBackupProbe.invalidCount);
        }
        const bool workerMayUseApplicationSlot =
            nxsync::cloudWorkerMayUseApplicationSlot(
                hasAutomaticLaunch,
                lifecycle.activeProcessId == 0,
                applicationSlotIsFree());
        if (gAutomationsAllowed
            && (lifecycle.activeProcessId == 0 || hasAutomaticLaunch)
            && cloudWorkerProcessId == 0
            && nxsync::cloudWorkerReleaseDelayElapsed(
                workerCheckNowNs, workerLaunchNotBeforeNs)
            && workerMayUseApplicationSlot
            && (!nxsync::loadPendingCloudOperations(
                    QueueRoot,
                    nxsync::storageEnvironmentKey(gStorageEnvironment)).empty()
                || hasPendingPreflight
                || hasPendingLaunchRestore
                || hasReadyBackupRequest)) {
            const std::uint64_t nowNs = workerCheckNowNs;
            const bool urgentWorkerRequest = hasPendingPreflight
                || hasPendingLaunchRestore
                || hasReadyBackupRequest;
            if (urgentWorkerRequest || nowNs >= nextCloudWorkerAttemptNs) {
                Result launchResult = MAKERESULT(Module_Libnx, LibnxError_BadInput);
                if (createCloudWorkerLock()) {
                    launchResult = launchCloudWorker(cloudWorkerProcessId);
                } else {
                    observerError = "Unable to create the cloud worker lock";
                }
                if (R_FAILED(launchResult)) {
                    cloudWorkerProcessId = 0;
                    cloudWorkerCrashPending = false;
                    removeCloudWorkerLock();
                    nextCloudWorkerAttemptNs = nowNs
                        + static_cast<std::uint64_t>(
                            CloudWorkerRetryDelaySeconds)
                            * 1'000'000'000ULL;
                    if (observerError.empty()) {
                        observerError = formatResult(
                            "Unable to start the cloud worker",
                            launchResult);
                    }
                    if (hasAutomaticLaunch) {
                        const std::string failure = observerError;
                        allowLaunch(
                            automaticLaunch,
                            failure + ": continuing with the local save",
                            observerError);
                    }
                } else {
                    cloudWorkerCrashPending = false;
                    cloudWorkerExitObserved = false;
                    observerError.clear();
                }
            }
        }
        publishStatus(
            lifecycle, lastQueuedProgramId, backupState,
            true,
            gAutomationsAllowed ? "observer-ready" : "scope-blocked",
            observerError,
            previousStatus);
    }

    eventClose(&processEvent);
    pglEventObserverClose(&observer);
    return 0;
}
