#include "nxsync/atomic_file.hpp"
#include "nxsync/sysmodule_status.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace nxsync {
namespace {

std::string escapeValue(const std::string& value) {
    std::string result;
    for (const char ch : value) {
        if (ch == '\\') result += "\\\\";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '=') result += "\\e";
        else result.push_back(ch);
    }
    return result;
}

bool unescapeValue(const std::string& value, std::string& result) {
    result.clear();
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') escaped = true;
        else if (escaped) {
            if (ch == '\\') result.push_back('\\');
            else if (ch == 'n') result.push_back('\n');
            else if (ch == 'r') result.push_back('\r');
            else if (ch == 'e') result.push_back('=');
            else return false;
            escaped = false;
        } else result.push_back(ch);
    }
    return !escaped;
}

bool parseSize(const std::string& value, std::size_t& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = static_cast<std::size_t>(parsed);
    return true;
}

bool parseUnsigned64(const std::string& value, std::uint64_t& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

} // namespace

bool validateSysmoduleStatus(
    const SysmoduleStatus& status,
    std::string& error) {
    if ((status.version < 1 || status.version > SysmoduleStatusVersion)
        || status.buildVersion.empty()
        || status.state.empty() || status.state.find('\n') != std::string::npos) {
        error = "Invalid sysmodule status";
        return false;
    }
    if (status.version >= 7
        && (status.storageEnvironment != "emummc"
            && status.storageEnvironment != "sysmmc"
            && status.storageEnvironment != "unknown")) {
        error = "Invalid NAND environment in sysmodule status";
        return false;
    }
    if (status.version >= 7
        && status.automationScope != "emummc"
        && status.automationScope != "all") {
        error = "Invalid automation scope in sysmodule status";
        return false;
    }
    error.clear();
    return true;
}

std::string serializeSysmoduleStatus(const SysmoduleStatus& status) {
    return "version=" + std::to_string(status.version) + "\n"
        + "build_version=" + escapeValue(status.buildVersion) + "\n"
        + "enabled=" + std::string(status.enabled ? "true" : "false") + "\n"
        + "process_id=" + std::to_string(status.processId) + "\n"
        + "preflight_enabled=" + std::string(status.preflightEnabled ? "true" : "false") + "\n"
        + "backup_on_game_exit=" + std::string(status.backupOnGameExit ? "true" : "false") + "\n"
        + "state=" + escapeValue(status.state) + "\n"
        + "pending_operations=" + std::to_string(status.pendingOperations) + "\n"
        + "last_error=" + escapeValue(status.lastError) + "\n"
        + "active_program_id=" + escapeValue(status.activeProgramId) + "\n"
        + "active_process_id=" + std::to_string(status.activeProcessId) + "\n"
        + "last_program_id=" + escapeValue(status.lastProgramId) + "\n"
        + "last_event=" + escapeValue(status.lastEvent) + "\n"
        + "event_sequence=" + std::to_string(status.eventSequence) + "\n"
        + "pending_backup_requests="
            + std::to_string(status.pendingBackupRequests) + "\n"
        + "last_queued_program_id="
            + escapeValue(status.lastQueuedProgramId) + "\n"
        + "last_backup_program_id="
            + escapeValue(status.lastBackupProgramId) + "\n"
        + "last_backup_result="
            + escapeValue(status.lastBackupResult) + "\n"
        + "last_backup_created="
            + std::to_string(status.lastBackupCreated) + "\n"
        + "last_backup_unchanged="
            + std::to_string(status.lastBackupUnchanged) + "\n"
        + "last_backup_archive_path="
            + escapeValue(status.lastBackupArchivePath) + "\n"
        + "backup_stage=" + escapeValue(status.backupStage) + "\n"
        + "backup_current_path=" + escapeValue(status.backupCurrentPath) + "\n"
        + "backup_files_processed="
            + std::to_string(status.backupFilesProcessed) + "\n"
        + "backup_total_files=" + std::to_string(status.backupTotalFiles) + "\n"
        + "backup_bytes_processed="
            + std::to_string(status.backupBytesProcessed) + "\n"
        + "backup_total_bytes=" + std::to_string(status.backupTotalBytes) + "\n"
        + "launch_gate_enabled="
            + std::string(status.launchGateEnabled ? "true" : "false") + "\n"
        + "launch_gate_state=" + escapeValue(status.launchGateState) + "\n"
        + "last_preflight_program_id="
            + escapeValue(status.lastPreflightProgramId) + "\n"
        + "last_preflight_process_id="
            + std::to_string(status.lastPreflightProcessId) + "\n"
        + "last_preflight_result="
            + escapeValue(status.lastPreflightResult) + "\n"
        + "preflight_sequence="
            + std::to_string(status.preflightSequence) + "\n"
        + "storage_environment="
            + escapeValue(status.storageEnvironment) + "\n"
        + "automation_scope=" + escapeValue(status.automationScope) + "\n"
        + "automations_allowed="
            + std::string(status.automationsAllowed ? "true" : "false") + "\n";
}

bool parseSysmoduleStatus(
    const std::string& text,
    SysmoduleStatus& status,
    std::string& error) {
    status = SysmoduleStatus{};
    bool versionSeen = false;
    bool buildSeen = false;
    bool enabledSeen = false;
    bool stateSeen = false;
    bool processIdSeen = false;
    bool preflightEnabledSeen = false;
    bool backupOnGameExitSeen = false;
    bool pendingSeen = false;
    bool errorSeen = false;
    bool activeProgramSeen = false;
    bool activeProcessSeen = false;
    bool lastProgramSeen = false;
    bool lastEventSeen = false;
    bool eventSequenceSeen = false;
    bool pendingBackupRequestsSeen = false;
    bool lastQueuedProgramSeen = false;
    bool lastBackupProgramSeen = false;
    bool lastBackupResultSeen = false;
    bool lastBackupCreatedSeen = false;
    bool lastBackupUnchangedSeen = false;
    bool lastBackupArchiveSeen = false;
    bool backupStageSeen = false;
    bool backupCurrentPathSeen = false;
    bool backupFilesSeen = false;
    bool backupTotalFilesSeen = false;
    bool backupBytesSeen = false;
    bool backupTotalBytesSeen = false;
    bool launchGateEnabledSeen = false;
    bool launchGateStateSeen = false;
    bool lastPreflightProgramSeen = false;
    bool lastPreflightProcessSeen = false;
    bool lastPreflightResultSeen = false;
    bool preflightSequenceSeen = false;
    bool storageEnvironmentSeen = false;
    bool automationScopeSeen = false;
    bool automationsAllowedSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            std::string value;
            if (!unescapeValue(line.substr(separator + 1), value)) {
                error = "Invalid escape sequence in sysmodule status";
                return false;
            }
            std::size_t number = 0;
            if (key == "version" && parseSize(value, number)) {
                status.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "build_version") {
                status.buildVersion = value;
                buildSeen = true;
            } else if (key == "enabled" && (value == "true" || value == "false")) {
                status.enabled = value == "true";
                enabledSeen = true;
            } else if (key == "process_id" && parseUnsigned64(value, status.processId)) {
                processIdSeen = true;
            } else if (key == "preflight_enabled" && (value == "true" || value == "false")) {
                status.preflightEnabled = value == "true";
                preflightEnabledSeen = true;
            } else if (key == "backup_on_game_exit" && (value == "true" || value == "false")) {
                status.backupOnGameExit = value == "true";
                backupOnGameExitSeen = true;
            } else if (key == "state") {
                status.state = value;
                stateSeen = true;
            } else if (key == "pending_operations" && parseSize(value, number)) {
                status.pendingOperations = number;
                pendingSeen = true;
            } else if (key == "last_error") {
                status.lastError = value;
                errorSeen = true;
            } else if (key == "active_program_id") {
                status.activeProgramId = value;
                activeProgramSeen = true;
            } else if (key == "active_process_id"
                && parseUnsigned64(value, status.activeProcessId)) {
                activeProcessSeen = true;
            } else if (key == "last_program_id") {
                status.lastProgramId = value;
                lastProgramSeen = true;
            } else if (key == "last_event") {
                status.lastEvent = value;
                lastEventSeen = true;
            } else if (key == "event_sequence"
                && parseUnsigned64(value, status.eventSequence)) {
                eventSequenceSeen = true;
            } else if (key == "pending_backup_requests"
                && parseSize(value, number)) {
                status.pendingBackupRequests = number;
                pendingBackupRequestsSeen = true;
            } else if (key == "last_queued_program_id") {
                status.lastQueuedProgramId = value;
                lastQueuedProgramSeen = true;
            } else if (key == "last_backup_program_id") {
                status.lastBackupProgramId = value;
                lastBackupProgramSeen = true;
            } else if (key == "last_backup_result") {
                status.lastBackupResult = value;
                lastBackupResultSeen = true;
            } else if (key == "last_backup_created" && parseSize(value, number)) {
                status.lastBackupCreated = number;
                lastBackupCreatedSeen = true;
            } else if (key == "last_backup_unchanged" && parseSize(value, number)) {
                status.lastBackupUnchanged = number;
                lastBackupUnchangedSeen = true;
            } else if (key == "last_backup_archive_path") {
                status.lastBackupArchivePath = value;
                lastBackupArchiveSeen = true;
            } else if (key == "backup_stage") {
                status.backupStage = value;
                backupStageSeen = true;
            } else if (key == "backup_current_path") {
                status.backupCurrentPath = value;
                backupCurrentPathSeen = true;
            } else if (key == "backup_files_processed" && parseSize(value, number)) {
                status.backupFilesProcessed = number;
                backupFilesSeen = true;
            } else if (key == "backup_total_files" && parseSize(value, number)) {
                status.backupTotalFiles = number;
                backupTotalFilesSeen = true;
            } else if (key == "backup_bytes_processed"
                && parseUnsigned64(value, status.backupBytesProcessed)) {
                backupBytesSeen = true;
            } else if (key == "backup_total_bytes"
                && parseUnsigned64(value, status.backupTotalBytes)) {
                backupTotalBytesSeen = true;
            } else if (key == "launch_gate_enabled"
                && (value == "true" || value == "false")) {
                status.launchGateEnabled = value == "true";
                launchGateEnabledSeen = true;
            } else if (key == "launch_gate_state") {
                status.launchGateState = value;
                launchGateStateSeen = true;
            } else if (key == "last_preflight_program_id") {
                status.lastPreflightProgramId = value;
                lastPreflightProgramSeen = true;
            } else if (key == "last_preflight_process_id"
                && parseUnsigned64(value, status.lastPreflightProcessId)) {
                lastPreflightProcessSeen = true;
            } else if (key == "last_preflight_result") {
                status.lastPreflightResult = value;
                lastPreflightResultSeen = true;
            } else if (key == "preflight_sequence"
                && parseUnsigned64(value, status.preflightSequence)) {
                preflightSequenceSeen = true;
            } else if (key == "storage_environment") {
                status.storageEnvironment = value;
                storageEnvironmentSeen = true;
            } else if (key == "automation_scope") {
                status.automationScope = value;
                automationScopeSeen = true;
            } else if (key == "automations_allowed"
                && (value == "true" || value == "false")) {
                status.automationsAllowed = value == "true";
                automationsAllowedSeen = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !buildSeen || !enabledSeen || !stateSeen
        || !pendingSeen || !errorSeen) {
        error = "Required sysmodule status fields are missing";
        return false;
    }
    if (status.version >= 2 && (!activeProgramSeen || !activeProcessSeen
        || !lastProgramSeen || !lastEventSeen || !eventSequenceSeen)) {
        error = "Lifecycle fields are missing from sysmodule status";
        return false;
    }
    if (status.version >= 3
        && (!pendingBackupRequestsSeen || !lastQueuedProgramSeen)) {
        error = "Backup queue fields are missing from sysmodule status";
        return false;
    }
    if (status.version >= 4 && (!lastBackupProgramSeen
        || !lastBackupResultSeen || !lastBackupCreatedSeen
        || !lastBackupUnchangedSeen || !lastBackupArchiveSeen)) {
        error = "Backup result fields are missing from sysmodule status";
        return false;
    }
    if (status.version >= 5 && (!backupStageSeen || !backupCurrentPathSeen
        || !backupFilesSeen || !backupTotalFilesSeen
        || !backupBytesSeen || !backupTotalBytesSeen)) {
        error = "Backup progress fields are missing from sysmodule status";
        return false;
    }
    if (status.version >= 6 && (!launchGateEnabledSeen || !launchGateStateSeen
        || !lastPreflightProgramSeen || !lastPreflightProcessSeen
        || !lastPreflightResultSeen || !preflightSequenceSeen)) {
        error = "Preflight fields are missing from sysmodule status";
        return false;
    }
    if (status.version >= 7 && (!storageEnvironmentSeen
        || !automationScopeSeen || !automationsAllowedSeen)) {
        error = "NAND environment fields are missing from sysmodule status";
        return false;
    }
    if (status.version >= 8 && (!processIdSeen
        || !preflightEnabledSeen || !backupOnGameExitSeen)) {
        error = "Automation settings fields are missing from sysmodule status";
        return false;
    }
    return validateSysmoduleStatus(status, error);
}

bool loadSysmoduleStatus(
    const std::string& path,
    SysmoduleStatus& status,
    std::string& error) {
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError, 4 * 1024 * 1024)) {
        error = "Sysmodule status was not found";
        return false;
    }
    return parseSysmoduleStatus(text, status, error);
}

} // namespace nxsync
