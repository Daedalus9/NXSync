#include "nxsync/automation_settings.hpp"

#include <cassert>
#include <string>

using namespace nxsync;

int main() {
    SysmoduleConfig fresh;
    assert(!automationRequested(fresh, AutomationFeature::Preflight));
    assert(!automationRequested(fresh, AutomationFeature::BackupOnExit));
    fresh.pollIntervalSeconds = 42;
    const auto enabled = withAutomationEnabled(fresh, AutomationFeature::Preflight, true);
    assert(enabled.enabled && enabled.preflightEnabled && enabled.backupOnGameExit);
    assert(enabled.emummcOnly && enabled.pollIntervalSeconds == 42);
    assert(!fresh.enabled); // Building a draft does not alter the saved config.
    const auto noPreflight = withAutomationEnabled(enabled, AutomationFeature::Preflight, false);
    assert(noPreflight.enabled && noPreflight.backupOnGameExit && !noPreflight.preflightEnabled);
    const auto noBackups = withAutomationEnabled(enabled, AutomationFeature::BackupOnExit, false);
    assert(noBackups.enabled && noBackups.preflightEnabled && !noBackups.backupOnGameExit);
    auto bothOff = withAutomationEnabled(noBackups, AutomationFeature::Preflight, false);
    assert(bothOff.enabled && !bothOff.preflightEnabled && !bothOff.backupOnGameExit);
    bothOff.emummcOnly = false;
    auto preflightOnly = withAutomationEnabled(bothOff, AutomationFeature::Preflight, true);
    assert(preflightOnly.preflightEnabled && !preflightOnly.backupOnGameExit);
    assert(!preflightOnly.emummcOnly && preflightOnly.pollIntervalSeconds == 42);
    auto backupsOnly = withAutomationEnabled(fresh, AutomationFeature::BackupOnExit, true);
    assert(backupsOnly.enabled && backupsOnly.backupOnGameExit && !backupsOnly.preflightEnabled);

    fresh.preflightEnabled = true;
    backupsOnly = withAutomationEnabled(fresh, AutomationFeature::BackupOnExit, true);
    assert(backupsOnly.backupOnGameExit && !backupsOnly.preflightEnabled);

    SysmoduleStatus status;
    status.processId = 123;
    status.enabled = true;
    status.preflightEnabled = true;
    status.backupOnGameExit = true;
    status.storageEnvironment = "emummc";
    status.automationScope = "emummc";
    status.automationsAllowed = true;
    status.state = "observer-ready";
    status.launchGateEnabled = true;
    status.launchGateState = "automatic-ready";

    SysmoduleConfig config = enabled;
    auto feature = AutomationFeature::Preflight;
    auto environment = StorageEnvironment::EmuMmc;
    bool cloud = true;
    bool query = true;
    std::uint64_t pid = 123;
    std::string missing;
    const SysmoduleStatus* observed = &status;
    const auto state = [&] {
        return describeAutomation(config, feature, environment, cloud, missing, query, pid, observed);
    };
    const auto expect = [&](const char* label) { assert(state().label == std::string(label)); };
    expect("Automatic");
    pid = 0; // A stale ready file cannot turn a stopped process into Automatic.
    expect("Restart required");
    status.state = "observer-error";
    status.lastError = "PGL initialization failed";
    expect("Module error");
    assert(state().detail.find(status.lastError) != std::string::npos);
    status.state = "observer-ready";
    pid = 124;
    expect("Starting");
    pid = 123;
    observed = nullptr;
    expect("Starting");
    observed = &status;
    query = false;
    expect("Status unavailable");
    query = true;
    status.version = 7;
    expect("Update required");
    status.version = SysmoduleStatusVersion;
    status.preflightEnabled = false;
    expect("Applying");
    status.preflightEnabled = true;
    config = noPreflight;
    expect("Applying");
    status.preflightEnabled = false;
    expect("Disabled");
    config = enabled;
    status.preflightEnabled = true;
    environment = StorageEnvironment::SysMmc;
    expect("Inactive on sysMMC");
    environment = StorageEnvironment::Unknown;
    expect("Environment unknown");
    environment = StorageEnvironment::EmuMmc;
    status.storageEnvironment = "sysmmc";
    expect("Starting");
    status.storageEnvironment = "emummc";
    missing = "NXSync boot flag";
    expect("Dependencies missing");
    assert(state().detail.find(missing) != std::string::npos);
    missing.clear();
    cloud = false;
    expect("Cloud not configured");
    cloud = true;
    status.state = "configuration-error";
    expect("Module error");
    status.state = "observer-ready";
    status.launchGateState = "withdrawn";
    status.lastPreflightResult = "Launch hook withdrawn";
    expect("Module error");
    status.launchGateState = "overlay-dependencies-missing";
    expect("Dependencies missing");
    status.launchGateState = "checking-cloud";
    expect("In progress");
    status.launchGateEnabled = false;
    expect("Applying");
    status.launchGateEnabled = true;
    status.launchGateState = "allowed";
    expect("Automatic");
    feature = AutomationFeature::BackupOnExit;
    status.launchGateState = "withdrawn";
    expect("Automatic"); // A preflight error does not disable exit backups.
    config = noBackups;
    expect("Applying");
    status.backupOnGameExit = false;
    expect("Disabled");
    config = enabled;
    expect("Applying");
    status.backupOnGameExit = true;
    expect("Automatic");
    return 0;
}
