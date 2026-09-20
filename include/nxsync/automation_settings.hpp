#pragma once

#include "nxsync/storage_environment.hpp"
#include "nxsync/sysmodule_config.hpp"
#include "nxsync/sysmodule_status.hpp"

namespace nxsync {

enum class AutomationFeature { Preflight, BackupOnExit };
enum class AutomationReadiness { Disabled, Pending, Ready, Error };

struct AutomationState {
    const char* label;
    std::string detail;
    AutomationReadiness readiness;
};

inline bool automationRequested(const SysmoduleConfig& config, AutomationFeature feature) {
    return config.enabled && (feature == AutomationFeature::Preflight
        ? config.preflightEnabled : config.backupOnGameExit);
}

inline SysmoduleConfig withAutomationEnabled(
    SysmoduleConfig config, AutomationFeature feature, bool enabled) {
    if (feature == AutomationFeature::Preflight) {
        config.preflightEnabled = enabled;
        // The first guided activation also opts into backups, as disclosed in
        // the confirmation. Later changes preserve the user's backup preference.
        if (enabled && !config.enabled) config.backupOnGameExit = true;
    } else {
        config.backupOnGameExit = enabled;
        // Opting into backups alone must not activate a stale preflight flag
        // left behind while the master switch was off.
        if (enabled && !config.enabled) config.preflightEnabled = false;
    }
    if (enabled) config.enabled = true;
    // Keep the observer alive when disabling one feature. In-flight restores
    // and queued work must finish, and the other feature remains independent.
    return config;
}

inline AutomationState describeAutomation(
    const SysmoduleConfig& config, AutomationFeature feature,
    StorageEnvironment environment, bool cloudConfigured,
    const std::string& missingDependencies, bool processQueryAvailable,
    std::uint64_t runningProcessId, const SysmoduleStatus* status) {
    const bool preflight = feature == AutomationFeature::Preflight;
    const bool requested = automationRequested(config, feature);
    const bool current = runningProcessId != 0 && status != nullptr
        && status->version >= 8 && status->processId == runningProcessId
        && status->storageEnvironment == storageEnvironmentKey(environment);
    if (!requested) {
        if (current && status->enabled
            && (preflight ? status->preflightEnabled : status->backupOnGameExit)) {
            return {"Applying", "Disabling new automatic work; queued work may finish",
                AutomationReadiness::Pending};
        }
        return {"Disabled", preflight
            ? "Press A to enable the check before a game starts"
            : "Press A to enable backups and uploads after games close",
            AutomationReadiness::Disabled};
    }
    if (environment == StorageEnvironment::Unknown) {
        return {"Environment unknown", "Cannot identify sysMMC or emuMMC; automation is inactive",
            AutomationReadiness::Error};
    }
    if (!automationScopeAllows(config.emummcOnly, environment)) {
        return {"Inactive on sysMMC", "Saved for emuMMC; boot into emuMMC to use automation",
            AutomationReadiness::Pending};
    }
    if (!missingDependencies.empty()) {
        return {"Dependencies missing", "Missing: " + missingDependencies, AutomationReadiness::Error};
    }
    if (!cloudConfigured) {
        return {"Cloud not configured", "Configure the Nextcloud account in Settings",
            AutomationReadiness::Error};
    }
    if (!processQueryAvailable) {
        return {"Status unavailable", "Unable to check whether the sysmodule is running",
            AutomationReadiness::Error};
    }
    if (runningProcessId == 0) {
        if (status && (status->state == "configuration-error" || status->state == "observer-error")) {
            return {"Module error", "Last reported: " + (status->lastError.empty()
                ? status->state : status->lastError) + "; fully restart Atmosphere after fixing it",
                AutomationReadiness::Error};
        }
        return {"Restart required", "Fully restart Atmosphere to start the sysmodule",
            AutomationReadiness::Pending};
    }
    if (status && status->version < 8) {
        return {"Update required", "Install matching NXSync components and restart Atmosphere",
            AutomationReadiness::Error};
    }
    if (!current) {
        return {"Starting", "Waiting for status from the running sysmodule",
            AutomationReadiness::Pending};
    }
    if (status->state == "configuration-error" || status->state == "observer-error") {
        return {"Module error", status->lastError.empty() ? status->state : status->lastError,
            AutomationReadiness::Error};
    }
    if (!status->enabled || !status->automationsAllowed
        || status->automationScope != (config.emummcOnly ? "emummc" : "all")
        || (preflight ? !status->preflightEnabled : !status->backupOnGameExit)) {
        return {"Applying", "Waiting for the sysmodule to apply the saved setting",
            AutomationReadiness::Pending};
    }
    if (preflight) {
        if (status->launchGateState == "unavailable" || status->launchGateState == "withdrawn") {
            return {"Module error", status->lastPreflightResult.empty()
                ? "Launch hook unavailable; check the installed Atmosphere payload"
                : status->lastPreflightResult, AutomationReadiness::Error};
        }
        if (status->launchGateState == "overlay-dependencies-missing") {
            return {"Dependencies missing", "The sysmodule cannot find the overlay dependencies",
                AutomationReadiness::Error};
        }
        if (!status->launchGateEnabled || status->launchGateState == "disabled") {
            return {"Applying", "Waiting for the launch check to become active",
                AutomationReadiness::Pending};
        }
        if (status->launchGateState != "automatic-ready" && status->launchGateState != "allowed") {
            return {"In progress", "Launch check: " + status->launchGateState,
                AutomationReadiness::Pending};
        }
    }
    return {"Automatic", preflight ? "Checks cloud saves before a game starts"
        : "Backs up changed saves and uploads them after a game closes",
        AutomationReadiness::Ready};
}

} // namespace nxsync
