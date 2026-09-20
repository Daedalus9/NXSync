#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nxsync {

constexpr unsigned SysmoduleStatusVersion = 8;

struct SysmoduleStatus {
    unsigned version{SysmoduleStatusVersion};
    std::string buildVersion;
    std::uint64_t processId{0};
    bool preflightEnabled{false};
    bool backupOnGameExit{false};
    bool enabled{false};
    std::string state;
    std::size_t pendingOperations{0};
    std::string lastError;
    std::string activeProgramId;
    std::uint64_t activeProcessId{0};
    std::string lastProgramId;
    std::string lastEvent;
    std::uint64_t eventSequence{0};
    std::size_t pendingBackupRequests{0};
    std::string lastQueuedProgramId;
    std::string lastBackupProgramId;
    std::string lastBackupResult;
    std::size_t lastBackupCreated{0};
    std::size_t lastBackupUnchanged{0};
    std::string lastBackupArchivePath;
    std::string backupStage;
    std::string backupCurrentPath;
    std::size_t backupFilesProcessed{0};
    std::size_t backupTotalFiles{0};
    std::uint64_t backupBytesProcessed{0};
    std::uint64_t backupTotalBytes{0};
    bool launchGateEnabled{false};
    std::string launchGateState;
    std::string lastPreflightProgramId;
    std::uint64_t lastPreflightProcessId{0};
    std::string lastPreflightResult;
    std::uint64_t preflightSequence{0};
    std::string storageEnvironment;
    std::string automationScope;
    bool automationsAllowed{false};
};

bool validateSysmoduleStatus(const SysmoduleStatus& status, std::string& error);
std::string serializeSysmoduleStatus(const SysmoduleStatus& status);
bool parseSysmoduleStatus(
    const std::string& text,
    SysmoduleStatus& status,
    std::string& error);
bool loadSysmoduleStatus(
    const std::string& path,
    SysmoduleStatus& status,
    std::string& error);

} // namespace nxsync
