#include "nxsync/sysmodule_config.hpp"
#include "nxsync/sysmodule_status.hpp"

#include <cassert>
#include <string>
#include <unistd.h>

int main() {
    nxsync::SysmoduleConfig config;
    config.enabled = true;
    config.backupOnGameExit = false;
    config.preflightEnabled = true;
    config.emummcOnly = true;
    config.pollIntervalSeconds = 30;
    std::string error;
    assert(nxsync::validateSysmoduleConfig(config, error));

    nxsync::SysmoduleConfig parsedConfig;
    assert(nxsync::parseSysmoduleConfig(
        nxsync::serializeSysmoduleConfig(config),
        parsedConfig,
        error));
    assert(parsedConfig.enabled);
    assert(parsedConfig.preflightEnabled);
    assert(parsedConfig.emummcOnly);
    assert(parsedConfig.pollIntervalSeconds == 30);
    assert(!parsedConfig.backupOnGameExit);
    parsedConfig.pollIntervalSeconds = 4;
    assert(!nxsync::validateSysmoduleConfig(parsedConfig, error));

    const std::string configPath = "/tmp/nxsync-sysmodule-config-"
        + std::to_string(static_cast<long long>(getpid())) + ".ini";
    int systemError = 0;
    assert(nxsync::writeSysmoduleConfig(configPath, config, systemError));
    assert(nxsync::loadSysmoduleConfig(configPath, parsedConfig, error));
    assert(parsedConfig.enabled);
    assert(parsedConfig.preflightEnabled);
    assert(parsedConfig.emummcOnly);

    const std::string legacyConfig =
        "version=1\nenabled=true\npoll_interval_seconds=30\n";
    assert(nxsync::parseSysmoduleConfig(legacyConfig, parsedConfig, error));
    assert(!parsedConfig.preflightEnabled);
    assert(parsedConfig.backupOnGameExit);
    assert(!nxsync::parseSysmoduleConfig(legacyConfig + "backup_on_game_exit=invalid\n", parsedConfig, error));
    assert(nxsync::parseSysmoduleConfig(legacyConfig + "backup_on_game_exit=false\n", parsedConfig, error));
    assert(!parsedConfig.backupOnGameExit);
    assert(parsedConfig.emummcOnly);
    const std::string allNandsConfig =
        "version=1\nenabled=true\npreflight_enabled=true\n"
        "automation_scope=all\npoll_interval_seconds=30\n";
    assert(nxsync::parseSysmoduleConfig(allNandsConfig, parsedConfig, error));
    assert(!parsedConfig.emummcOnly);
    assert(unlink(configPath.c_str()) == 0);

    nxsync::SysmoduleStatus status;
    status.buildVersion = "0.6.0-dev";
    status.enabled = true;
    status.processId = 9876;
    status.preflightEnabled = true;
    status.backupOnGameExit = false;
    status.state = "observer-ready";
    status.pendingOperations = 2;
    status.lastError = "network=unavailable\nretrying";
    status.activeProgramId = "0100A3D008C5C000";
    status.activeProcessId = 1234;
    status.lastProgramId = status.activeProgramId;
    status.lastEvent = "start";
    status.eventSequence = 7;
    status.pendingBackupRequests = 2;
    status.lastQueuedProgramId = "0100A3D008C5C000";
    status.lastBackupProgramId = status.lastQueuedProgramId;
    status.lastBackupResult = "created";
    status.lastBackupCreated = 1;
    status.lastBackupUnchanged = 0;
    status.lastBackupArchivePath = "sdmc:/switch/NXSync/backups/test.zip";
    status.backupStage = "Creating ZIP archive";
    status.backupCurrentPath = "save/data.bin";
    status.backupFilesProcessed = 3;
    status.backupTotalFiles = 9;
    status.backupBytesProcessed = 1024;
    status.backupTotalBytes = 4096;
    status.launchGateEnabled = true;
    status.launchGateState = "armed";
    status.lastPreflightProgramId = status.activeProgramId;
    status.lastPreflightProcessId = status.activeProcessId;
    status.lastPreflightResult = "resumed";
    status.preflightSequence = 1;
    status.storageEnvironment = "emummc";
    status.automationScope = "emummc";
    status.automationsAllowed = true;
    assert(nxsync::validateSysmoduleStatus(status, error));
    nxsync::SysmoduleStatus parsedStatus;
    assert(nxsync::parseSysmoduleStatus(
        nxsync::serializeSysmoduleStatus(status),
        parsedStatus,
        error));
    assert(parsedStatus.buildVersion == status.buildVersion);
    assert(parsedStatus.enabled);
    assert(parsedStatus.processId == 9876);
    assert(parsedStatus.preflightEnabled);
    assert(!parsedStatus.backupOnGameExit);
    assert(parsedStatus.state == status.state);
    assert(parsedStatus.pendingOperations == 2);
    assert(parsedStatus.lastError == status.lastError);
    assert(parsedStatus.activeProgramId == status.activeProgramId);
    assert(parsedStatus.activeProcessId == status.activeProcessId);
    assert(parsedStatus.lastEvent == "start");
    assert(parsedStatus.eventSequence == 7);
    assert(parsedStatus.pendingBackupRequests == 2);
    assert(parsedStatus.lastQueuedProgramId == status.lastQueuedProgramId);
    assert(parsedStatus.lastBackupResult == "created");
    assert(parsedStatus.lastBackupCreated == 1);
    assert(parsedStatus.lastBackupArchivePath == status.lastBackupArchivePath);
    assert(parsedStatus.backupStage == status.backupStage);
    assert(parsedStatus.backupFilesProcessed == 3);
    assert(parsedStatus.backupBytesProcessed == 1024);
    assert(parsedStatus.launchGateEnabled);
    assert(parsedStatus.launchGateState == "armed");
    assert(parsedStatus.lastPreflightResult == "resumed");
    assert(parsedStatus.preflightSequence == 1);
    assert(parsedStatus.storageEnvironment == "emummc");
    assert(parsedStatus.automationScope == "emummc");
    assert(parsedStatus.automationsAllowed);

    for (const auto& field : {"process_id=9876\n", "preflight_enabled=true\n", "backup_on_game_exit=false\n"}) {
        auto incomplete = nxsync::serializeSysmoduleStatus(status);
        const auto offset = incomplete.find(field);
        assert(offset != std::string::npos);
        incomplete.erase(offset, std::string(field).size());
        assert(!nxsync::parseSysmoduleStatus(incomplete, parsedStatus, error));
        // Legacy status remains readable by diagnostics, but cannot acknowledge settings.
        incomplete.replace(0, std::string("version=8").size(), "version=7");
        assert(nxsync::parseSysmoduleStatus(incomplete, parsedStatus, error));
    }

    const std::string legacyStatus =
        "version=1\nbuild_version=0.1.1-dev\nenabled=true\n"
        "state=observer-ready\npending_operations=0\nlast_error=\n";
    assert(nxsync::parseSysmoduleStatus(legacyStatus, parsedStatus, error));
    assert(parsedStatus.version == 1);
    assert(parsedStatus.activeProgramId.empty());

    const std::string lifecycleStatus =
        "version=2\nbuild_version=0.2.0-dev\nenabled=true\n"
        "state=observer-ready\npending_operations=0\nlast_error=\n"
        "active_program_id=\nactive_process_id=0\n"
        "last_program_id=0100A3D008C5C000\nlast_event=exit\n"
        "event_sequence=2\n";
    assert(nxsync::parseSysmoduleStatus(lifecycleStatus, parsedStatus, error));
    assert(parsedStatus.version == 2);
    assert(parsedStatus.pendingBackupRequests == 0);
    return 0;
}
