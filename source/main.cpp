#include <switch.h>

#include "nxsync/app_config.hpp"
#include "nxsync/automation_settings.hpp"
#include "nxsync/backup_manager.hpp"
#include "nxsync/backup_request_queue.hpp"
#include "nxsync/backup_state.hpp"
#include "nxsync/cloud_index.hpp"
#include "nxsync/cloud_queue.hpp"
#include "nxsync/cloud_worker_status.hpp"
#include "nxsync/device_identity.hpp"
#include "nxsync/game_version.hpp"
#include "nxsync/launch_protocol.hpp"
#include "nxsync/gui.hpp"
#include "nxsync/nextcloud_client.hpp"
#include "nxsync/nextcloud_login.hpp"
#include "nxsync/nextcloud_paths.hpp"
#include "nxsync/credential_crypto.hpp"
#include "nxsync/overlay_dependencies.hpp"
#include "nxsync/overlay_catalog.hpp"
#include "nxsync/preflight_protocol.hpp"
#include "nxsync/remote_layout.hpp"
#include "nxsync/retention.hpp"
#include "nxsync/revision_compare.hpp"
#include "nxsync/restore_manager.hpp"
#include "nxsync/save_catalog.hpp"
#include "nxsync/storage_environment.hpp"
#include "nxsync/sync_engine.hpp"
#include "nxsync/sysmodule_config.hpp"
#include "nxsync/sysmodule_status.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <functional>
#include <map>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* ConfigPath = "sdmc:/config/NXSync/config.ini";
constexpr const char* FallbackIdPath = "sdmc:/config/NXSync/device_id.txt";
constexpr const char* AppVersion = "0.30.12-rc2";
constexpr std::size_t SavesPerPage = 9;
constexpr const char* LocalBackupRoot = "sdmc:/switch/NXSync/backups";
constexpr const char* CloudQueueRoot = "sdmc:/config/NXSync/queue";
constexpr const char* BackupRequestRoot = "sdmc:/config/NXSync/backup-requests";
constexpr const char* SysmoduleStatusPath = "sdmc:/config/NXSync/sysmodule.status";
constexpr const char* SysmoduleConfigPath = "sdmc:/config/NXSync/sysmodule.ini";
constexpr const char* CloudWorkerStatusPath = "sdmc:/config/NXSync/cloud-worker.status";
constexpr const char* CloudWorkerLockPath = "sdmc:/config/NXSync/cloud-worker.lock";
constexpr const char* PreflightRequestPath = "sdmc:/config/NXSync/preflight.request";
constexpr const char* PreflightStatusPath = "sdmc:/config/NXSync/preflight.status";
constexpr const char* OverlayCatalogPath = "sdmc:/config/NXSync/overlay.catalog";

bool cloudWorkerLocked() {
    struct stat fileStat{};
    return stat(CloudWorkerLockPath, &fileStat) == 0
        && S_ISREG(fileStat.st_mode);
}

struct ConfigWizardResult {
    bool saved{false};
    std::string message;
    bool testConnection{false};
    bool exitRequested{false};
};

struct BackupUiContext {
    nxsync::Gui* gui{nullptr};
    const nxsync::DeviceIdentity* identity{nullptr};
    const nxsync::UserSaves* user{nullptr};
    const nxsync::SaveEntry* save{nullptr};
    std::size_t batchIndex{0};
    std::size_t batchTotal{0};
    std::uint64_t lastUploadRenderedBytes{0};
};

struct BatchBackupSummary {
    std::size_t succeeded{0};
    std::size_t skipped{0};
    std::size_t failed{0};
    std::size_t uploaded{0};
    std::size_t uploadFailed{0};
    std::size_t indexed{0};
    std::size_t indexFailed{0};
    std::size_t localPruned{0};
    std::size_t remotePruned{0};
    std::size_t retentionFailed{0};
    std::string firstError;
    std::string lastArchivePath;
};

struct RestoreUiResult {
    bool exitRequested{false};
    bool catalogChanged{false};
    bool success{false};
    std::string message;
    std::string safetyBackupPath;
};

struct LocalRestoreEntry {
    std::string archivePath;
    std::string device;
    std::string profile;
    std::string filename;
    std::uint64_t size{0};
};

enum class MenuAction {
    Selected,
    Back,
    Exit,
};

using BackupStateCache = std::vector<std::vector<nxsync::LocalBackupState>>;

std::string trimInput(std::string value) {
    const auto notSpace = [](const unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool hasZipExtension(const std::string& value) {
    if (value.size() < 4) {
        return false;
    }
    std::string extension = value.substr(value.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".zip";
}

bool parseTitleIdFolder(const std::string& value, std::uint64_t& titleId) {
    if (value.size() != 16) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 16);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed == 0) {
        return false;
    }
    titleId = static_cast<std::uint64_t>(parsed);
    return true;
}

bool waitForButtonRelease(PadState& pad, const u64 buttonMask) {
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtons(&pad) & buttonMask) == 0) {
            return true;
        }
        svcSleepThread(5'000'000LL);
    }
    return false;
}

MenuAction chooseMenuItem(
    nxsync::Gui& gui,
    nxsync::GuiMenuView& view,
    std::size_t& selectedIndex,
    const std::function<void()>& refresh = {}) {
    constexpr std::size_t RowsPerPage = 7;
    selectedIndex = std::min(
        selectedIndex,
        view.items.empty() ? 0 : view.items.size() - 1);
    bool redraw = true;
    PadState menuPad;
    padInitializeDefault(&menuPad);
    std::uint64_t lastRefreshNs = armTicksToNs(armGetSystemTick());
    while (appletMainLoop()) {
        const std::uint64_t nowNs = armTicksToNs(armGetSystemTick());
        if (refresh && nowNs - lastRefreshNs >= 1'000'000'000ULL) {
            refresh();
            lastRefreshNs = nowNs;
            redraw = true;
        }
        if (redraw) {
            view.selected = selectedIndex;
            view.page = selectedIndex / RowsPerPage;
            view.pageCount = std::max<std::size_t>(
                1,
                (view.items.size() + RowsPerPage - 1) / RowsPerPage);
            gui.renderMenu(view);
            redraw = false;
        }
        padUpdate(&menuPad);
        const u64 down = padGetButtonsDown(&menuPad);
        if ((down & HidNpadButton_Plus) != 0) {
            return MenuAction::Exit;
        }
        if ((down & HidNpadButton_B) != 0) {
            if (!waitForButtonRelease(menuPad, HidNpadButton_B)) {
                return MenuAction::Exit;
            }
            return MenuAction::Back;
        }
        if (view.items.empty()) {
            continue;
        }
        if ((down & HidNpadButton_A) != 0) {
            // The catalog and modal menus use different PadState instances.
            // Consume the selection completely so A cannot become a new press
            // when control returns to the underlying catalog.
            if (!waitForButtonRelease(menuPad, HidNpadButton_A)) {
                return MenuAction::Exit;
            }
            return MenuAction::Selected;
        }
        if ((down & HidNpadButton_Up) != 0) {
            selectedIndex = selectedIndex == 0
                ? view.items.size() - 1
                : selectedIndex - 1;
            redraw = true;
        } else if ((down & HidNpadButton_Down) != 0) {
            selectedIndex = (selectedIndex + 1) % view.items.size();
            redraw = true;
        } else if ((down & HidNpadButton_Left) != 0) {
            selectedIndex = selectedIndex < RowsPerPage
                ? 0
                : selectedIndex - RowsPerPage;
            redraw = true;
        } else if ((down & HidNpadButton_Right) != 0) {
            selectedIndex = std::min(
                selectedIndex + RowsPerPage,
                view.items.size() - 1);
            redraw = true;
        }
    }
    return MenuAction::Exit;
}

bool confirmRestore(
    nxsync::Gui& gui,
    const std::string& device,
    const nxsync::RestoreInspection& inspection,
    const nxsync::UserSaves& destination,
    bool& exitRequested) {
    nxsync::GuiMenuView view;
    view.version = AppVersion;
    view.device = device;
    view.title = "Confirm restore";
    view.breadcrumb = inspection.manifest.sourceDevice + " / "
        + inspection.manifest.sourceProfile + " / "
        + nxsync::formatTitleId(inspection.manifest.titleId);
    view.instruction = "The source profile is shown for information only.";
    view.destructiveConfirmation = true;
    view.confirmationTitle = inspection.manifest.titleName.empty()
        ? nxsync::formatTitleId(inspection.manifest.titleId)
        : inspection.manifest.titleName;
    view.confirmationDetail = "Source: " + inspection.manifest.sourceProfile
        + "  →  Local destination: " + destination.nickname
        + ". Existing local data will be protected first.";

    PadState confirmationPad;
    padInitializeDefault(&confirmationPad);
    while (appletMainLoop()) {
        gui.renderMenu(view);
        padUpdate(&confirmationPad);
        const u64 down = padGetButtonsDown(&confirmationPad);
        const u64 held = padGetButtons(&confirmationPad);
        if ((down & HidNpadButton_Plus) != 0) {
            exitRequested = true;
            return false;
        }
        if ((down & HidNpadButton_B) != 0) {
            if (!waitForButtonRelease(confirmationPad, HidNpadButton_B)) {
                exitRequested = true;
            }
            return false;
        }
        if ((down & HidNpadButton_A) != 0
            && (held & HidNpadButton_ZL) != 0
            && (held & HidNpadButton_ZR) != 0) {
            if (!waitForButtonRelease(
                    confirmationPad,
                    HidNpadButton_A | HidNpadButton_ZL | HidNpadButton_ZR)) {
                exitRequested = true;
                return false;
            }
            return true;
        }
    }
    exitRequested = true;
    return false;
}

bool showTextInput(
    const char* header,
    const char* subText,
    const char* guideText,
    const std::string& initialText,
    const std::size_t maximumLength,
    const bool password,
    const char* okButtonText,
    std::string& output) {
    SwkbdConfig keyboard;
    const Result createResult = swkbdCreate(&keyboard, 0);
    if (R_FAILED(createResult)) {
        return false;
    }

    if (password) {
        swkbdConfigMakePresetPassword(&keyboard);
    } else {
        swkbdConfigMakePresetDefault(&keyboard);
    }
    swkbdConfigSetHeaderText(&keyboard, header);
    swkbdConfigSetSubText(&keyboard, subText);
    swkbdConfigSetGuideText(&keyboard, guideText);
    swkbdConfigSetOkButtonText(&keyboard, okButtonText);
    swkbdConfigSetStringLenMax(&keyboard, static_cast<s32>(maximumLength));
    swkbdConfigSetStringLenMin(&keyboard, password ? 0 : 1);
    if (!initialText.empty()) {
        swkbdConfigSetInitialText(&keyboard, initialText.c_str());
    }

    std::vector<char> buffer(maximumLength + 1, '\0');
    const Result showResult = swkbdShow(&keyboard, buffer.data(), buffer.size());
    swkbdClose(&keyboard);
    if (R_FAILED(showResult)) {
        return false;
    }
    output.assign(buffer.data());
    return true;
}

ConfigWizardResult runManualNextcloudConfigWizard(nxsync::AppConfig& config) {
    nxsync::AppConfig draft = config;
    std::string value;

    if (!showTextInput(
            "NXSync - URL Nextcloud",
            "Enter the HTTPS address of the instance or personal WebDAV endpoint.",
            "Example: https://cloud.example.com/nextcloud",
            draft.nextcloudUrl,
            480,
            false,
            "Next",
            value)) {
        return {false, "Nextcloud configuration cancelled"};
    }
    draft.nextcloudUrl = trimInput(value);

    if (!showTextInput(
            "NXSync - Nextcloud username",
            "Enter the username used to access Nextcloud.",
            "Username",
            draft.nextcloudUsername,
            128,
            false,
            "Next",
            value)) {
        return {false, "Nextcloud configuration cancelled"};
    }
    draft.nextcloudUsername = trimInput(value);

    const bool hasSavedPassword = !draft.nextcloudAppPassword.empty();
    if (!showTextInput(
            "NXSync - application password",
            hasSavedPassword
                ? "Leave blank to keep the saved password."
                : "Enter a revocable application password.",
            "Do not use the account's main password",
            std::string(),
            256,
            true,
            "Next",
            value)) {
        return {false, "Nextcloud configuration cancelled"};
    }
    if (!value.empty()) {
        draft.nextcloudAppPassword = value;
        draft.credentialError.clear();
        draft.credentialEncrypted = false;
    }

    if (!showTextInput(
            "NXSync - remote folder",
            "Backups will be separated by console inside this folder.",
            "Example: NXSync",
            draft.remoteRoot,
            128,
            false,
            "Save",
            value)) {
        return {false, "Nextcloud configuration cancelled"};
    }
    draft.remoteRoot = trimInput(value);

    std::string error;
    if (!nxsync::saveAppConfig(ConfigPath, draft, error)) {
        return {false, error};
    }
    config = draft;
    return {true, "Nextcloud configuration saved", true};
}

ConfigWizardResult runQrNextcloudConfigWizard(
    nxsync::Gui& gui, const std::string& device, nxsync::AppConfig& config) {
    nxsync::AppConfig draft = config;
    struct ClearPassword {
        std::string& value;
        ~ClearPassword() { nxsync::secureClearString(value); }
    } clear{draft.nextcloudAppPassword};
    nxsync::secureClearString(draft.nextcloudAppPassword);
    std::string server, value;
    if (!showTextInput("NXSync - Nextcloud server", "Enter the HTTPS address of your Nextcloud instance.",
            "Example: https://cloud.example.com/nextcloud", nxsync::normalizeLoginServer(config.nextcloudUrl),
            480, false, "Next", value)) return {false, "Nextcloud connection cancelled"};
    server = nxsync::normalizeLoginServer(trimInput(value));
    if (server.empty()) return {false, "Invalid Nextcloud address. Use HTTPS without credentials or query parameters."};
    if (!showTextInput("NXSync - remote folder", "Backups will be separated by console inside this folder.",
            "Example: NXSync", draft.remoteRoot, 128, false, "Connect", value))
        return {false, "Nextcloud connection cancelled"};
    draft.remoteRoot = trimInput(value);

    const auto nowMs = [] { return armTicksToNs(armGetSystemTick()) / 1'000'000ULL; };
    nxsync::NextcloudLogin login;
    login.begin(server, nowMs());
    nxsync::GuiLoginView view;
    view.version = AppVersion; view.device = device; view.server = server;
    PadState pad;
    padInitializeDefault(&pad);
    std::string qrUrl, saveError, lastStatus;
    ClearPassword clearQrUrl{qrUrl};
    unsigned lastSeconds = ~0U;
    bool retrySave = true;
    while (appletMainLoop()) {
        const auto now = nowMs();
        login.update(now);
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if ((down & HidNpadButton_Plus) != 0) return {false, "Exit requested", false, true};
        if ((down & HidNpadButton_B) != 0) {
            login.cancel();
            return {false, "Nextcloud connection cancelled", false, !waitForButtonRelease(pad, HidNpadButton_B)};
        }
        const bool failed = login.state() == nxsync::LoginState::Error || login.state() == nxsync::LoginState::Expired;
        if ((down & HidNpadButton_A) != 0 && (failed || !saveError.empty())) {
            if (!waitForButtonRelease(pad, HidNpadButton_A)) return {false, "Exit requested", false, true};
            if (failed) login.begin(server, nowMs());
            retrySave = true; saveError.clear(); lastSeconds = ~0U;
        }
        if (login.state() == nxsync::LoginState::Authorized && retrySave) {
            const auto& account = login.credentials();
            draft.nextcloudUrl = nxsync::makeNextcloudDavBaseUrl(account.server, account.davUserId);
            draft.nextcloudUsername = account.loginName;
            draft.nextcloudAppPassword = account.appPassword;
            draft.credentialError.clear(); draft.credentialEncrypted = false;
            if (nxsync::saveAppConfig(ConfigPath, draft, saveError)) {
                nxsync::secureClearString(config.nextcloudAppPassword);
                config = draft;
                return {true, "Nextcloud connected with QR", true};
            }
            nxsync::secureClearString(draft.nextcloudAppPassword);
            retrySave = false;
        }
        if (qrUrl != login.loginUrl()) {
            nxsync::secureClearString(qrUrl);
            qrUrl = login.loginUrl();
            view.qr = nxsync::makeLoginQrCode(qrUrl);
            if (!qrUrl.empty() && view.qr.size == 0)
                return {false, "The login URL is too long for a readable QR code. Use manual setup."};
            lastSeconds = ~0U;
        }
        view.status = saveError.empty() ? login.message() : saveError;
        view.secondsRemaining = login.secondsRemaining(now);
        view.saveRetry = !saveError.empty();
        view.canRetry = view.saveRetry || failed;
        if (view.status != lastStatus || view.secondsRemaining != lastSeconds) {
            gui.renderLogin(view);
            lastStatus = view.status; lastSeconds = view.secondsRemaining;
        }
        svcSleepThread(16'000'000LL);
    }
    return {false, "Exit requested", false, true};
}

ConfigWizardResult runNextcloudConfigWizard(
    nxsync::Gui& gui, const std::string& device, nxsync::AppConfig& config) {
    nxsync::GuiMenuView view;
    view.version = AppVersion; view.device = device; view.title = "Connect to Nextcloud";
    view.breadcrumb = "Settings / Nextcloud account";
    view.instruction = "Choose how to connect. Your current account is kept until the new one is saved.";
    view.items = {
        {"Connect with QR", "Sign in on your phone and grant access to NXSync", "Recommended", nxsync::GuiTone::Accent},
        {"Manual setup", "Enter a WebDAV URL, username and application password", "", nxsync::GuiTone::Neutral}};
    std::size_t selected = 0;
    const auto action = chooseMenuItem(gui, view, selected);
    if (action == MenuAction::Exit) return {false, "Exit requested", false, true};
    if (action == MenuAction::Back) return {false, "Nextcloud configuration cancelled"};
    return selected == 0 ? runQrNextcloudConfigWizard(gui, device, config) : runManualNextcloudConfigWizard(config);
}

struct AutomationSnapshot {
    nxsync::SysmoduleConfig config;
    nxsync::SysmoduleStatus status;
    bool configAvailable{false};
    bool statusAvailable{false};
    bool processQueryAvailable{false};
    u64 runningProcessId{0};
    std::string configError;
    std::string missingCore;
    std::string missingPreflight;
    nxsync::StorageEnvironment environment{nxsync::StorageEnvironment::Unknown};
};

AutomationSnapshot readAutomationSnapshot() {
    AutomationSnapshot snapshot;
    snapshot.configAvailable = nxsync::loadSysmoduleConfig(
        SysmoduleConfigPath, snapshot.config, snapshot.configError);
    std::string error;
    snapshot.statusAvailable = nxsync::loadSysmoduleStatus(
        SysmoduleStatusPath, snapshot.status, error);
    snapshot.environment = nxsync::detectCurrentStorageEnvironment();
    if (R_SUCCEEDED(pmdmntInitialize())) {
        snapshot.processQueryAvailable = true;
        // A stopped sysmodule has no PID. Status files alone survive restarts.
        u64 processId = 0;
        if (R_SUCCEEDED(pmdmntGetProcessId(&processId, 0x4200000000004E58ULL))) {
            snapshot.runningProcessId = processId;
        }
        pmdmntExit();
    }
    const auto missing = [](std::string& text, const char* label) {
        if (!text.empty()) text += ", ";
        text += label;
    };
    if (!nxsync::overlayDependencyFileExists(
            "sdmc:/atmosphere/contents/4200000000004E58/exefs.nsp", true)) {
        missing(snapshot.missingCore, "NXSync sysmodule");
    }
    if (!nxsync::overlayDependencyFileExists(
            "sdmc:/atmosphere/contents/4200000000004E58/flags/boot2.flag", false)) {
        missing(snapshot.missingCore, "NXSync boot flag");
    }
    if (!nxsync::overlayDependencyFileExists(
            "sdmc:/atmosphere/contents/4200000000004E59/exefs.nsp", true)) {
        missing(snapshot.missingCore, "NXSync worker");
    }
    snapshot.missingPreflight = snapshot.missingCore;
    const auto dependencies = nxsync::detectOverlayDependencies();
    if (!dependencies.ready()) {
        missing(snapshot.missingPreflight,
            nxsync::missingOverlayDependencySummary(dependencies).c_str());
    }
    if (!nxsync::overlayDependencyFileExists("sdmc:/switch/.overlays/NXSync.ovl", true)) {
        missing(snapshot.missingPreflight, "NXSync overlay");
    }
    return snapshot;
}

nxsync::GuiMenuItem automationMenuItem(
    const AutomationSnapshot& snapshot, const nxsync::AppConfig& appConfig,
    nxsync::AutomationFeature feature) {
    const auto state = snapshot.configAvailable
        ? nxsync::describeAutomation(snapshot.config, feature, snapshot.environment,
            appConfig.nextcloudConfigured(), feature == nxsync::AutomationFeature::Preflight
                ? snapshot.missingPreflight : snapshot.missingCore,
            snapshot.processQueryAvailable, snapshot.runningProcessId,
            snapshot.statusAvailable ? &snapshot.status : nullptr)
        : nxsync::AutomationState{"Configuration error", snapshot.configError,
            nxsync::AutomationReadiness::Error};
    const auto tone = state.readiness == nxsync::AutomationReadiness::Ready
        ? nxsync::GuiTone::Good : state.readiness == nxsync::AutomationReadiness::Error
        ? nxsync::GuiTone::Danger : state.readiness == nxsync::AutomationReadiness::Pending
        ? nxsync::GuiTone::Warning : nxsync::GuiTone::Neutral;
    return {feature == nxsync::AutomationFeature::Preflight
        ? "Game launch preflight" : "Automatic backups after game exit",
        state.detail, state.label, tone};
}

ConfigWizardResult changeAutomation(
    nxsync::Gui& gui, const std::string& device, const nxsync::AppConfig& appConfig,
    nxsync::AutomationFeature feature) {
    const auto snapshot = readAutomationSnapshot();
    if (!snapshot.configAvailable) return {false, snapshot.configError};
    const bool enable = !nxsync::automationRequested(snapshot.config, feature);
    const bool preflight = feature == nxsync::AutomationFeature::Preflight;
    if (enable) {
        if (!appConfig.nextcloudConfigured()) {
            return {false, "Configure the Nextcloud account before enabling automation"};
        }
        const auto& missing = preflight ? snapshot.missingPreflight : snapshot.missingCore;
        if (!missing.empty()) return {false, "Cannot enable automation. Missing: " + missing};
    }
    if (!preflight && snapshot.runningProcessId != 0 && snapshot.statusAvailable
        && snapshot.status.version < 8) {
        return {false, "Update NXSync and restart Atmosphere before changing exit backups"};
    }
    if (enable && !snapshot.config.enabled) {
        nxsync::GuiMenuView confirmation;
        confirmation.version = AppVersion;
        confirmation.device = device;
        confirmation.title = preflight ? "Enable game launch preflight?"
            : "Enable backups after game exit?";
        confirmation.instruction = "A - Enable    B - Cancel";
        confirmation.breadcrumb = snapshot.config.emummcOnly
            ? "Applies to emuMMC only; inactive on sysMMC."
            : "Applies to both sysMMC and emuMMC.";
        confirmation.items.push_back({"Enable",
            preflight ? "Also enables automatic backups and uploads after games close"
                : "Automatically backs up changed saves and uploads them after games close",
            "", nxsync::GuiTone::Accent});
        if (snapshot.processQueryAvailable && snapshot.runningProcessId == 0) {
            confirmation.instruction += ". A full restart of Atmosphere is required.";
        }
        std::size_t selected = 0;
        const auto action = chooseMenuItem(gui, confirmation, selected);
        if (action == MenuAction::Exit) return {false, "Exit requested", false, true};
        if (action != MenuAction::Selected) return {false, "Automation activation cancelled"};
    }
    const auto draft = nxsync::withAutomationEnabled(snapshot.config, feature, enable);
    int systemError = 0;
    if (!nxsync::writeSysmoduleConfig(SysmoduleConfigPath, draft, systemError)) {
        return {false, "Unable to save automation settings (errno "
            + std::to_string(systemError) + ")"};
    }
    if (!enable) {
        return {true, preflight ? "Preflight disabled; the backup setting is unchanged"
            : "Exit backups disabled; queued work may finish. Preflight is unchanged"};
    }
    if (!nxsync::automationScopeAllows(draft.emummcOnly, snapshot.environment)) {
        return {true, snapshot.environment == nxsync::StorageEnvironment::SysMmc
            ? "Saved for emuMMC; automation remains inactive on sysMMC"
            : "Saved; automation is inactive until the NAND environment can be identified"};
    }
    if (!snapshot.processQueryAvailable) {
        return {true, "Saved; unable to verify the running module. Check Settings before playing"};
    }
    return {true, snapshot.runningProcessId == 0
        ? "Saved. Fully restart Atmosphere, then check for Automatic in Settings"
        : "Saved. The running sysmodule will apply the setting; check Settings for its status"};
}

ConfigWizardResult runSettingsMenu(
    nxsync::Gui& gui,
    const std::string& device,
    nxsync::AppConfig& config) {
    nxsync::GuiMenuView view;
    view.version = AppVersion;
    view.device = device;
    view.title = "NXSync settings";
    view.breadcrumb = "Nextcloud and automation";
    view.instruction = "Select an item with A; B returns to the catalog.";
    view.items.push_back(nxsync::GuiMenuItem{
        "Nextcloud account",
        !config.credentialError.empty()
            ? config.credentialError
            : (config.nextcloudConfigured()
            ? config.nextcloudUsername + " - " + config.nextcloudUrl
            : "Connect with your phone using QR, or enter credentials manually"),
        !config.credentialError.empty()
            ? "Credential error"
            : (config.nextcloudConfigured() ? "Encrypted" : "Not configured"),
        config.nextcloudConfigured() ? nxsync::GuiTone::Good : nxsync::GuiTone::Warning});
    view.items.push_back(nxsync::GuiMenuItem{
        "Automatic backup at startup",
        config.autoBackupOnStart
            ? "Backs up changed saves and retries pending uploads"
            : "No automatic operation when NXSync starts",
        config.autoBackupOnStart ? "Enabled" : "Disabled",
        config.autoBackupOnStart ? nxsync::GuiTone::Good : nxsync::GuiTone::Neutral});
    view.items.push_back(nxsync::GuiMenuItem{
        "Backup retention",
        config.retentionCount == 0
            ? "Does not automatically delete any archive"
            : "Keeps the latest " + std::to_string(config.retentionCount)
                + " ZIP files per console, profile and game",
        config.retentionCount == 0
            ? "Disabled"
            : std::to_string(config.retentionCount),
        config.retentionCount == 0 ? nxsync::GuiTone::Neutral : nxsync::GuiTone::Good});
    const std::vector<nxsync::PendingCloudOperation> queuedOperations =
        nxsync::SyncEngine(CloudQueueRoot).pendingOperations();
    const std::vector<nxsync::PendingBackupRequest> queuedBackups =
        nxsync::loadPendingBackupRequests(
            BackupRequestRoot,
            nxsync::storageEnvironmentKey(
                nxsync::detectCurrentStorageEnvironment()));
    view.items.push_back(nxsync::GuiMenuItem{
        "Cloud queue",
        queuedOperations.empty()
            ? "No pending uploads or index updates"
            : "Press A to inspect errors and pending operations",
        queuedOperations.empty()
            ? "Empty"
            : std::to_string(queuedOperations.size()) + " pending",
        queuedOperations.empty() ? nxsync::GuiTone::Good : nxsync::GuiTone::Warning});
    nxsync::SysmoduleStatus sysmoduleStatus;
    std::string sysmoduleError;
    const bool sysmoduleAvailable = nxsync::loadSysmoduleStatus(
        SysmoduleStatusPath,
        sysmoduleStatus,
        sysmoduleError);
    nxsync::CloudWorkerStatus cloudWorkerStatus;
    std::string cloudWorkerError;
    const bool cloudWorkerAvailable = nxsync::loadCloudWorkerStatus(
        CloudWorkerStatusPath,
        cloudWorkerStatus,
        cloudWorkerError);
    view.items.push_back(nxsync::GuiMenuItem{
        "Sysmodule (observer)",
        sysmoduleAvailable
            ? "Build " + sysmoduleStatus.buildVersion + " - queue entries: "
                + std::to_string(sysmoduleStatus.pendingOperations)
                + " - backup requests: "
                + std::to_string(sysmoduleStatus.pendingBackupRequests)
                + (sysmoduleStatus.activeProgramId.empty()
                    ? " - no active game"
                    : " - active " + sysmoduleStatus.activeProgramId)
            : "No status published by the headless process",
        sysmoduleAvailable ? sysmoduleStatus.state : "Not detected",
        !sysmoduleAvailable
            ? nxsync::GuiTone::Neutral
            : (sysmoduleStatus.state == "configuration-error"
                || sysmoduleStatus.state == "observer-error"
                ? nxsync::GuiTone::Danger
                : (sysmoduleStatus.enabled
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Neutral))});
    AutomationSnapshot automation = readAutomationSnapshot();
    view.items.push_back(automationMenuItem(automation, config, nxsync::AutomationFeature::Preflight));
    view.items.push_back(automationMenuItem(automation, config, nxsync::AutomationFeature::BackupOnExit));
    const auto refreshAutomation = [&]() {
        automation = readAutomationSnapshot();
        view.items[5] = automationMenuItem(automation, config, nxsync::AutomationFeature::Preflight);
        view.items[6] = automationMenuItem(automation, config, nxsync::AutomationFeature::BackupOnExit);
        if (!automation.processQueryAvailable) {
            view.items[4].badge = "Status unavailable";
            view.items[4].secondary = "Unable to query the running sysmodule";
            view.items[4].tone = nxsync::GuiTone::Warning;
        } else if (automation.runningProcessId != 0) {
            const bool current = automation.statusAvailable && automation.status.version >= 8
                && automation.status.processId == automation.runningProcessId;
            view.items[4].badge = current ? automation.status.state : "Starting / update required";
            view.items[4].secondary = current
                ? "Build " + automation.status.buildVersion + " - " + automation.status.lastError
                : "Waiting for status from a matching NXSync sysmodule";
            view.items[4].tone = current && automation.status.state == "observer-ready"
                ? nxsync::GuiTone::Good : nxsync::GuiTone::Warning;
        } else {
            view.items[4].badge = "Stopped";
            view.items[4].secondary = "The sysmodule is not running; stored diagnostics may be from an earlier boot";
            view.items[4].tone = nxsync::GuiTone::Neutral;
        }
    };
    refreshAutomation();

    std::size_t selected = 0;
    const MenuAction action = chooseMenuItem(gui, view, selected, refreshAutomation);
    if (action == MenuAction::Exit) {
        return {false, "Exit requested", false, true};
    }
    if (action == MenuAction::Back) {
        return {false, "Settings cancelled"};
    }
    if (selected == 0) {
        return runNextcloudConfigWizard(gui, device, config);
    }
    if (selected == 2) {
        static constexpr std::size_t Choices[] = {0, 3, 5, 10};
        nxsync::GuiMenuView retentionMenu;
        retentionMenu.version = AppVersion;
        retentionMenu.device = device;
        retentionMenu.title = "Backup retention";
        retentionMenu.breadcrumb = "Limit per console / profile / game";
        retentionMenu.instruction =
            "Cleanup starts with the next verified backup. B cancels.";
        std::size_t choiceIndex = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            if (Choices[index] == config.retentionCount) {
                choiceIndex = index;
            }
            retentionMenu.items.push_back(nxsync::GuiMenuItem{
                Choices[index] == 0
                    ? "Disabled"
                    : "Keep " + std::to_string(Choices[index]) + " backups",
                Choices[index] == 0
                    ? "Do not automatically delete any ZIP archive"
                    : "Delete only the oldest NXSync archives",
                Choices[index] == config.retentionCount ? "Current" : std::string(),
                Choices[index] == config.retentionCount
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Neutral});
        }
        const MenuAction retentionAction = chooseMenuItem(
            gui,
            retentionMenu,
            choiceIndex);
        if (retentionAction == MenuAction::Exit) {
            return {false, "Exit requested", false, true};
        }
        if (retentionAction == MenuAction::Back) {
            return {false, "Settings cancelled"};
        }
        nxsync::AppConfig draft = config;
        draft.retentionCount = Choices[choiceIndex];
        std::string error;
        if (!nxsync::saveAppConfig(ConfigPath, draft, error)) {
            return {false, error};
        }
        config = draft;
        return {
            true,
            config.retentionCount == 0
                ? "Automatic retention disabled"
                : "Retention set to "
                    + std::to_string(config.retentionCount)
                    + " backups per game; it will apply from the next backup"};
    }
    if (selected == 3) {
        nxsync::GuiMenuView queueMenu;
        queueMenu.version = AppVersion;
        queueMenu.device = device;
        queueMenu.title = "Persistent cloud queue";
        queueMenu.breadcrumb = std::to_string(queuedOperations.size())
            + " pending operations";
        queueMenu.instruction = queuedOperations.empty()
            ? "The queue is empty. Press B to return."
            : "Press Y twice to retry the queue. Press B to return to the catalog.";
        for (const nxsync::PendingCloudOperation& operation : queuedOperations) {
            queueMenu.items.push_back(nxsync::GuiMenuItem{
                operation.titleId + " / " + operation.revisionId.substr(0, 12),
                operation.lastError.empty()
                    ? operation.remotePath
                    : operation.lastError,
                std::to_string(operation.attemptCount) + " attempts",
                operation.lastError.empty()
                    ? nxsync::GuiTone::Warning
                    : nxsync::GuiTone::Danger});
        }
        std::size_t queueSelection = 0;
        const MenuAction queueAction = chooseMenuItem(gui, queueMenu, queueSelection);
        if (queueAction == MenuAction::Exit) {
            return {false, "Exit requested", false, true};
        }
        return {false, queuedOperations.empty()
            ? "Cloud queue is empty"
            : "Cloud queue: press Y twice to retry"};
    }
    if (selected == 4) {
        nxsync::GuiMenuView statusMenu;
        statusMenu.version = AppVersion;
        statusMenu.device = device;
        statusMenu.title = "NXSync sysmodule";
        statusMenu.breadcrumb = sysmoduleAvailable
            ? "Build " + sysmoduleStatus.buildVersion
            : "Observer not detected";
        statusMenu.instruction =
            "Local observer and transient cloud worker; B returns to settings.";
        statusMenu.items.push_back(nxsync::GuiMenuItem{
            sysmoduleAvailable ? sysmoduleStatus.state : "Not installed or not running",
            sysmoduleAvailable
                ? (sysmoduleStatus.lastError.empty()
                    ? "Valid configuration; pending operations seen: "
                        + std::to_string(sysmoduleStatus.pendingOperations)
                    : sysmoduleStatus.lastError)
                : sysmoduleError,
            sysmoduleAvailable
                ? (sysmoduleStatus.enabled ? "Enabled" : "Disabled")
                : "No status",
            sysmoduleAvailable && sysmoduleStatus.enabled
                ? nxsync::GuiTone::Good
                : nxsync::GuiTone::Neutral});
        if (sysmoduleAvailable && sysmoduleStatus.version >= 2) {
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "Active application",
                sysmoduleStatus.activeProgramId.empty()
                    ? "No running game detected"
                    : "Process ID "
                        + std::to_string(sysmoduleStatus.activeProcessId),
                sysmoduleStatus.activeProgramId.empty()
                    ? "None"
                    : sysmoduleStatus.activeProgramId,
                sysmoduleStatus.activeProgramId.empty()
                    ? nxsync::GuiTone::Neutral
                    : nxsync::GuiTone::Good});
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "Last application event",
                sysmoduleStatus.lastProgramId.empty()
                    ? "No event recorded since the sysmodule started"
                    : sysmoduleStatus.lastProgramId + " - sequence "
                        + std::to_string(sysmoduleStatus.eventSequence),
                sysmoduleStatus.lastEvent.empty()
                    ? "None"
                    : sysmoduleStatus.lastEvent,
                nxsync::GuiTone::Neutral});
        }
        if (sysmoduleAvailable && sysmoduleStatus.version >= 3) {
            const std::uint64_t nowMonotonicNs = armTicksToNs(armGetSystemTick());
            if (queuedBackups.empty()) {
                statusMenu.items.push_back(nxsync::GuiMenuItem{
                    "Backup requests",
                    "No game closure has been queued yet",
                    "Empty",
                    nxsync::GuiTone::Neutral});
            }
            for (const nxsync::PendingBackupRequest& request : queuedBackups) {
                const bool ready = nxsync::isBackupRequestReady(
                    request,
                    nowMonotonicNs);
                std::uint64_t remainingSeconds = 0;
                if (!ready && request.notBeforeMonotonicNs > nowMonotonicNs) {
                    remainingSeconds = (request.notBeforeMonotonicNs
                        - nowMonotonicNs + 999'999'999ULL) / 1'000'000'000ULL;
                }
                statusMenu.items.push_back(nxsync::GuiMenuItem{
                    "Queued backup: " + request.titleId,
                    (request.lastError.empty()
                        ? "Event " + request.triggerEvent
                        : request.lastError)
                        + " - sequence "
                        + std::to_string(request.eventSequence)
                        + " - safety delay "
                        + std::to_string(request.settleDelaySeconds) + " s"
                        + (request.attemptCount == 0
                            ? std::string()
                            : " - attempts "
                                + std::to_string(request.attemptCount)),
                    ready
                        ? "Ready"
                        : (request.lastError.empty() ? "Waiting " : "Retry ")
                            + std::to_string(remainingSeconds) + " s",
                    !request.lastError.empty()
                        ? nxsync::GuiTone::Danger
                        : (ready ? nxsync::GuiTone::Good : nxsync::GuiTone::Warning)});
            }
        }
        if (sysmoduleAvailable && sysmoduleStatus.version >= 4
            && !sysmoduleStatus.lastBackupProgramId.empty()) {
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "Last headless backup",
                sysmoduleStatus.lastBackupProgramId
                    + " - created "
                    + std::to_string(sysmoduleStatus.lastBackupCreated)
                    + " - unchanged "
                    + std::to_string(sysmoduleStatus.lastBackupUnchanged)
                    + (sysmoduleStatus.lastBackupArchivePath.empty()
                        ? std::string()
                        : " - " + sysmoduleStatus.lastBackupArchivePath),
                sysmoduleStatus.lastBackupResult,
                sysmoduleStatus.lastBackupResult == "error"
                    ? nxsync::GuiTone::Danger
                    : nxsync::GuiTone::Good});
        }
        if (sysmoduleAvailable && sysmoduleStatus.version >= 5
            && sysmoduleStatus.state == "backup-working") {
            std::string progressDescription = sysmoduleStatus.backupStage.empty()
                ? "Preparing backup"
                : sysmoduleStatus.backupStage;
            if (!sysmoduleStatus.backupCurrentPath.empty()) {
                progressDescription += " - " + sysmoduleStatus.backupCurrentPath;
            }
            std::string progressBadge;
            if (sysmoduleStatus.backupTotalBytes > 0) {
                progressBadge = nxsync::formatByteSize(
                    sysmoduleStatus.backupBytesProcessed)
                    + " / "
                    + nxsync::formatByteSize(sysmoduleStatus.backupTotalBytes);
            } else if (sysmoduleStatus.backupTotalFiles > 0) {
                progressBadge = std::to_string(sysmoduleStatus.backupFilesProcessed)
                    + " / "
                    + std::to_string(sysmoduleStatus.backupTotalFiles)
                    + " file";
            } else {
                progressBadge = "Preparing";
            }
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "Backup progress",
                progressDescription
                    + " - leave and reopen this screen to refresh",
                progressBadge,
                sysmoduleStatus.state == "backup-working"
                    ? nxsync::GuiTone::Warning
                    : nxsync::GuiTone::Neutral});
        }
        if (sysmoduleAvailable && sysmoduleStatus.version >= 6) {
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "Automatic launch gate",
                sysmoduleStatus.lastPreflightProgramId.empty()
                    ? "Waiting for a game selected from HOME"
                    : sysmoduleStatus.lastPreflightProgramId
                        + " - PID "
                        + std::to_string(sysmoduleStatus.lastPreflightProcessId)
                        + " - sequence "
                        + std::to_string(sysmoduleStatus.preflightSequence),
                sysmoduleStatus.launchGateEnabled
                    ? sysmoduleStatus.launchGateState
                    : "Disabled",
                sysmoduleStatus.launchGateState == "error"
                    ? nxsync::GuiTone::Danger
                    : (sysmoduleStatus.launchGateState == "unavailable"
                        || sysmoduleStatus.launchGateState == "withdrawn"
                        ? nxsync::GuiTone::Neutral
                    : (sysmoduleStatus.launchGateState == "automatic-ready"
                        ? nxsync::GuiTone::Good
                    : (sysmoduleStatus.launchGateEnabled
                        ? nxsync::GuiTone::Warning
                        : nxsync::GuiTone::Neutral)))});
            if (!sysmoduleStatus.lastPreflightResult.empty()) {
                statusMenu.items.push_back(nxsync::GuiMenuItem{
                    "Last preflight",
                    "Cloud comparison and restore before game launch",
                    sysmoduleStatus.lastPreflightResult,
                    nxsync::GuiTone::Neutral});
            }
        }
        if (sysmoduleAvailable && sysmoduleStatus.version >= 7) {
            const std::string environment =
                sysmoduleStatus.storageEnvironment == "emummc"
                ? "emuMMC"
                : (sysmoduleStatus.storageEnvironment == "sysmmc"
                    ? "sysMMC"
                    : "Unknown");
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "NAND environment",
                sysmoduleStatus.automationScope == "emummc"
                    ? "Configured scope: emuMMC only"
                    : "Configured scope: emuMMC and sysMMC",
                environment,
                sysmoduleStatus.storageEnvironment == "unknown"
                    ? nxsync::GuiTone::Warning
                    : nxsync::GuiTone::Neutral});
            statusMenu.items.push_back(nxsync::GuiMenuItem{
                "Launch/close automation",
                sysmoduleStatus.automationsAllowed
                    ? "Launch gate, headless backup and automatic worker are allowed"
                    : "No automatic gate, backup or upload in this environment",
                sysmoduleStatus.automationsAllowed ? "Active" : "Blocked",
                sysmoduleStatus.automationsAllowed
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Neutral});
        }
        std::string workerBadge = "Never started";
        nxsync::GuiTone workerTone = nxsync::GuiTone::Neutral;
        if (cloudWorkerAvailable) {
            if (cloudWorkerStatus.state == "upload-working"
                && cloudWorkerStatus.totalBytes > 0) {
                workerBadge = nxsync::formatByteSize(
                    cloudWorkerStatus.bytesTransferred)
                    + " / "
                    + nxsync::formatByteSize(cloudWorkerStatus.totalBytes);
                workerTone = nxsync::GuiTone::Warning;
            } else if (cloudWorkerStatus.state == "completed") {
                workerBadge = std::to_string(cloudWorkerStatus.completed)
                    + " completed";
                workerTone = nxsync::GuiTone::Good;
            } else if (cloudWorkerStatus.state == "error") {
                workerBadge = std::to_string(cloudWorkerStatus.failed)
                    + " errors";
                workerTone = nxsync::GuiTone::Danger;
            } else {
                workerBadge = cloudWorkerStatus.state;
                workerTone = nxsync::GuiTone::Warning;
            }
        }
        statusMenu.items.push_back(nxsync::GuiMenuItem{
            cloudWorkerAvailable
                ? "Worker cloud " + cloudWorkerStatus.buildVersion
                : "Worker cloud",
            cloudWorkerAvailable
                ? cloudWorkerStatus.message
                    + (cloudWorkerStatus.revisionId.empty()
                        ? std::string()
                        : " - " + cloudWorkerStatus.revisionId)
                : cloudWorkerError,
            workerBadge,
            workerTone});
        std::size_t statusSelection = 0;
        const MenuAction statusAction = chooseMenuItem(gui, statusMenu, statusSelection);
        if (statusAction == MenuAction::Exit) {
            return {false, "Exit requested", false, true};
        }
        return {false, sysmoduleAvailable
            ? "Sysmodule " + sysmoduleStatus.state
            : "Sysmodule not detected"};
    }
    if (selected == 5 || selected == 6) {
        return changeAutomation(gui, device, config, selected == 5
            ? nxsync::AutomationFeature::Preflight : nxsync::AutomationFeature::BackupOnExit);
    }
    if (selected != 1) {
        return {false, "Settings cancelled"};
    }
    if (!config.autoBackupOnStart && !config.nextcloudConfigured()) {
        return {false, "Configure Nextcloud before enabling automatic backup"};
    }

    nxsync::AppConfig draft = config;
    draft.autoBackupOnStart = !draft.autoBackupOnStart;
    std::string error;
    if (!nxsync::saveAppConfig(ConfigPath, draft, error)) {
        return {false, error};
    }
    config = draft;
    return {
        true,
        config.autoBackupOnStart
            ? "Automatic backup at startup enabled"
            : "Automatic backup at startup disabled"};
}

std::size_t pageCount(const nxsync::UserSaves& user) {
    return std::max<std::size_t>(
        1,
        (user.saves.size() + SavesPerPage - 1) / SavesPerPage);
}

BackupStateCache loadBackupStateCache(
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog) {
    BackupStateCache cache;
    cache.reserve(catalog.users.size());
    for (const nxsync::UserSaves& user : catalog.users) {
        std::vector<nxsync::LocalBackupState> userStates;
        userStates.reserve(user.saves.size());
        for (const nxsync::SaveEntry& save : user.saves) {
            userStates.push_back(nxsync::findCurrentLocalBackup(identity, user, save));
        }
        cache.push_back(std::move(userStates));
    }
    return cache;
}

void refreshBackupState(
    BackupStateCache& cache,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog,
    const std::size_t userIndex,
    const std::size_t saveIndex) {
    if (userIndex >= cache.size() || userIndex >= catalog.users.size()
        || saveIndex >= cache[userIndex].size()
        || saveIndex >= catalog.users[userIndex].saves.size()) {
        return;
    }
    cache[userIndex][saveIndex] = nxsync::findCurrentLocalBackup(
        identity,
        catalog.users[userIndex],
        catalog.users[userIndex].saves[saveIndex]);
}

std::string backupDateFromArchivePath(const std::string& archivePath) {
    const std::size_t separator = archivePath.find_last_of('/');
    const std::string fileName = separator == std::string::npos
        ? archivePath
        : archivePath.substr(separator + 1);
    if (fileName.size() < 18
        || fileName[4] != '-'
        || fileName[7] != '-'
        || fileName[10] != 'T'
        || fileName[17] != 'Z') {
        return std::string();
    }
    const int year = std::atoi(fileName.substr(0, 4).c_str());
    const int month = std::atoi(fileName.substr(5, 2).c_str());
    const int day = std::atoi(fileName.substr(8, 2).c_str());
    const int hour = std::atoi(fileName.substr(11, 2).c_str());
    const int minute = std::atoi(fileName.substr(13, 2).c_str());
    std::time_t now = std::time(nullptr);
    std::tm currentUtc{};
    const bool currentYearAvailable = gmtime_r(&now, &currentUtc) != nullptr;
    const int currentYear = currentYearAvailable ? currentUtc.tm_year + 1900 : 0;
    if (month < 1 || month > 12 || day < 1 || day > 31
        || hour < 0 || hour > 23 || minute < 0 || minute > 59
        || (currentYear >= 2020 && (year < 2020 || year > currentYear + 1))) {
        return "unreliable date";
    }
    return fileName.substr(8, 2) + "/" + fileName.substr(5, 2)
        + "/" + fileName.substr(0, 4) + " "
        + fileName.substr(11, 2) + ":" + fileName.substr(13, 2) + " UTC";
}

std::string statusWithBackupDate(
    const std::string& status,
    const nxsync::LocalBackupState& state) {
    const std::string date = backupDateFromArchivePath(state.archivePath);
    return date.empty() ? status : status + " - " + date;
}

void renderBackupProgress(const nxsync::BackupProgress& progress, void* rawContext) {
    const auto* context = static_cast<const BackupUiContext*>(rawContext);
    if (context == nullptr || context->gui == nullptr) {
        return;
    }

    nxsync::GuiProgressView view;
    view.title = std::string(AppVersion) + " - local backup";
    if (context->identity != nullptr) {
        view.device = context->identity->folderName;
    }
    if (context->user != nullptr) {
        view.profile = context->user->nickname;
    }
    if (context->save != nullptr) {
        const std::string title = context->save->titleName.empty()
            ? nxsync::formatTitleId(context->save->applicationId)
            : context->save->titleName;
        view.game = title;
    }
    if (context->batchTotal > 0) {
        view.batch = "Game " + std::to_string(context->batchIndex)
            + "/" + std::to_string(context->batchTotal);
        view.batchCurrent = context->batchIndex;
        view.batchTotal = context->batchTotal;
    }
    view.stage = progress.stage;
    if (!progress.currentPath.empty()) {
        view.path = progress.currentPath;
    }
    if (progress.totalBytes > 0) {
        view.current = progress.bytesProcessed;
        view.total = progress.totalBytes;
    } else if (progress.totalFiles > 0) {
        view.current = progress.filesProcessed;
        view.total = progress.totalFiles;
    }
    if (progress.totalFiles > 0) {
        view.amount = std::to_string(progress.filesProcessed) + "/"
            + std::to_string(progress.totalFiles) + " file | "
            + nxsync::formatByteSize(progress.bytesProcessed) + " / "
            + nxsync::formatByteSize(progress.totalBytes);
    } else {
        view.amount = "Preparing operation...";
    }
    context->gui->renderProgress(view);
}

void renderNextcloudProgress(
    const nxsync::NextcloudProgress& progress,
    void* rawContext) {
    auto* context = static_cast<BackupUiContext*>(rawContext);
    if (context != nullptr && progress.totalBytes > 0
        && progress.bytesTransferred > 0
        && progress.bytesTransferred < progress.totalBytes
        && progress.bytesTransferred - context->lastUploadRenderedBytes < 1024 * 1024) {
        return;
    }
    if (context != nullptr) {
        context->lastUploadRenderedBytes = progress.bytesTransferred;
    }

    if (context == nullptr || context->gui == nullptr) {
        return;
    }

    nxsync::GuiProgressView view;
    view.title = std::string(AppVersion) + " - Nextcloud";
    if (context->identity != nullptr) {
        view.device = context->identity->folderName;
    }
    if (context->user != nullptr) {
        view.profile = context->user->nickname;
    }
    if (context->save != nullptr) {
        const std::string title = context->save->titleName.empty()
            ? nxsync::formatTitleId(context->save->applicationId)
            : context->save->titleName;
        view.game = title;
    }
    if (context->batchTotal > 0) {
        view.batch = "Game " + std::to_string(context->batchIndex)
            + "/" + std::to_string(context->batchTotal);
        view.batchCurrent = context->batchIndex;
        view.batchTotal = context->batchTotal;
    }
    view.stage = progress.stage;
    view.path = progress.remotePath;
    view.current = progress.bytesTransferred;
    view.total = progress.totalBytes;
    if (progress.totalBytes > 0) {
        const unsigned percentage = static_cast<unsigned>(
            (progress.bytesTransferred * 100ULL) / progress.totalBytes);
        view.amount = "Transferred: " + nxsync::formatByteSize(progress.bytesTransferred)
            + " / " + nxsync::formatByteSize(progress.totalBytes)
            + " (" + std::to_string(percentage) + "%)";
    } else {
        view.amount = "Preparing transfer...";
    }
    context->gui->renderProgress(view);
}

std::string describeBackupError(const nxsync::BackupResult& backup) {
    std::string description = backup.message;
    if (R_FAILED(backup.mountResult)) {
        description += " (" + nxsync::formatResult(backup.mountResult) + ")";
    } else if (backup.systemError != 0) {
        description += " (errno " + std::to_string(backup.systemError) + ")";
    }
    return description;
}

std::string fileNameFromPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string formatProfileUid(const AccountUid& uid) {
    char buffer[33]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llX%016llX",
        static_cast<unsigned long long>(uid.uid[1]),
        static_cast<unsigned long long>(uid.uid[0]));
    return buffer;
}

bool exportOverlayCatalog(
    const nxsync::SaveCatalog& saveCatalog,
    int& systemError) {
    nxsync::OverlayCatalog overlayCatalog;
    const std::time_t now = std::time(nullptr);
    overlayCatalog.generatedUnix = now > 0
        ? static_cast<std::uint64_t>(now)
        : 0;
    for (const nxsync::UserSaves& user : saveCatalog.users) {
        if (!user.registeredProfile) continue;
        const std::string profileName = user.nickname.empty()
            ? "Local profile"
            : user.nickname;
        for (const nxsync::SaveEntry& save : user.saves) {
            const std::string titleId = nxsync::formatTitleId(save.applicationId);
            overlayCatalog.entries.push_back(nxsync::OverlayCatalogEntry{
                formatProfileUid(user.uid),
                profileName,
                titleId,
                save.titleName.empty() ? "Title " + titleId : save.titleName});
        }
    }
    return nxsync::writeOverlayCatalogAtomic(
        OverlayCatalogPath,
        overlayCatalog,
        systemError);
}

std::string archiveCreatedUtc(const nxsync::LocalBackupState& state) {
    if (!state.createdUtc.empty()) {
        return state.createdUtc;
    }
    const std::string filename = fileNameFromPath(state.archivePath);
    return filename.size() >= 18 && filename[10] == 'T' && filename[17] == 'Z'
        ? filename.substr(0, 18)
        : std::string("unknown-time");
}

std::string backupRemotePath(
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::UserSaves& user,
    const nxsync::SaveEntry& save,
    const std::string& archivePath) {
    return nxsync::makeBackupRemotePath(
        config.remoteRoot,
        identity.folderName,
        formatProfileUid(user.uid),
        nxsync::formatTitleId(save.applicationId),
        fileNameFromPath(archivePath));
}

nxsync::SyncUploadPlan makeSyncUploadPlan(
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::UserSaves& user,
    const nxsync::SaveEntry& save,
    const nxsync::LocalBackupState& state) {
    nxsync::SyncSaveDescriptor saveDescriptor;
    saveDescriptor.remoteRoot = config.remoteRoot;
    saveDescriptor.deviceId = identity.folderName;
    saveDescriptor.profileName = user.nickname;
    saveDescriptor.profileUid = formatProfileUid(user.uid);
    saveDescriptor.titleId = nxsync::formatTitleId(save.applicationId);
    saveDescriptor.titleName = save.titleName;
    saveDescriptor.gameVersion = save.gameVersion;
    saveDescriptor.saveDataId = nxsync::formatTitleId(save.saveDataId);

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
        ? state.uncompressedBytes
        : save.rawSize;
    revision.fileCount = state.fileCount;
    revision.markLocalState = save.extraDataAvailable;
    return nxsync::SyncEngine(CloudQueueRoot).planUpload(
        saveDescriptor,
        revision);
}

struct RetentionRunResult {
    std::size_t localRemoved{0};
    std::size_t remoteRemoved{0};
    std::size_t failed{0};
    std::string firstError;
};

std::string parentPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

RetentionRunResult applyRetention(
    const nxsync::AppConfig& config,
    const nxsync::LocalBackupState& state,
    const std::string& verifiedRemotePath,
    nxsync::NextcloudClient* client,
    const bool pruneRemote,
    BackupUiContext& backupUi) {
    RetentionRunResult result;
    if (config.retentionCount == 0
        || !state.current
        || !state.recordValid
        || state.archivePath.empty()
        || backupUi.user == nullptr) {
        return result;
    }

    const std::string protectedFilename = fileNameFromPath(state.archivePath);
    nxsync::BackupProgress localProgress;
    localProgress.stage = "Applying local retention";
    localProgress.currentPath = parentPath(state.archivePath);
    renderBackupProgress(localProgress, &backupUi);
    const nxsync::RetentionResult local = nxsync::isProfileBackupDirectory(
        parentPath(state.archivePath), formatProfileUid(backupUi.user->uid)) ? nxsync::pruneLocalBackups(
        parentPath(state.archivePath),
        protectedFilename,
        config.retentionCount) : nxsync::RetentionResult{};
    result.localRemoved = local.removed;
    result.failed += local.failed;
    result.firstError = local.firstError;

    if (!pruneRemote || client == nullptr || verifiedRemotePath.empty()) {
        return result;
    }
    const std::string remoteDirectory = parentPath(verifiedRemotePath);
    if (!nxsync::isProfileBackupDirectory(remoteDirectory, formatProfileUid(backupUi.user->uid))) return result;
    nxsync::NextcloudProgress remoteProgress;
    remoteProgress.stage = "Applying Nextcloud retention";
    remoteProgress.remotePath = remoteDirectory;
    renderNextcloudProgress(remoteProgress, &backupUi);
    const nxsync::NextcloudListResult listing = client->listDirectory(remoteDirectory);
    if (!listing.success) {
        ++result.failed;
        if (result.firstError.empty()) {
            result.firstError = "Cloud retention: " + listing.message;
        }
        return result;
    }

    std::vector<std::string> filenames;
    for (const nxsync::NextcloudEntry& entry : listing.entries) {
        if (!entry.directory) {
            filenames.push_back(entry.name);
        }
    }
    const std::vector<std::string> toDelete = nxsync::selectBackupsToPrune(
        filenames,
        config.retentionCount,
        fileNameFromPath(verifiedRemotePath));
    for (const std::string& filename : toDelete) {
        const auto entry = std::find_if(
            listing.entries.begin(),
            listing.entries.end(),
            [&](const nxsync::NextcloudEntry& candidate) {
                return !candidate.directory && candidate.name == filename;
            });
        if (entry == listing.entries.end()) {
            continue;
        }
        remoteProgress.remotePath = entry->remotePath;
        renderNextcloudProgress(remoteProgress, &backupUi);
        const nxsync::NextcloudResult deletion = client->deleteFile(entry->remotePath);
        if (deletion.success) {
            ++result.remoteRemoved;
        } else {
            ++result.failed;
            if (result.firstError.empty()) {
                result.firstError = "Cloud retention: " + deletion.message;
            }
        }
    }
    return result;
}

void mergeRetentionResult(
    BatchBackupSummary& summary,
    const RetentionRunResult& retention) {
    summary.localPruned += retention.localRemoved;
    summary.remotePruned += retention.remoteRemoved;
    summary.retentionFailed += retention.failed;
    if (summary.firstError.empty() && !retention.firstError.empty()) {
        summary.firstError = retention.firstError;
    }
}

bool remoteBackupMatches(
    nxsync::NextcloudClient& client,
    const std::string& remotePath,
    const std::uint64_t expectedSize) {
    const std::size_t separator = remotePath.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= remotePath.size()) {
        return false;
    }
    const std::string directory = separator == 0
        ? "/"
        : remotePath.substr(0, separator);
    const std::string filename = remotePath.substr(separator + 1);
    const nxsync::NextcloudListResult listing = client.listDirectory(directory);
    if (!listing.success) {
        return false;
    }
    return std::any_of(
        listing.entries.begin(),
        listing.entries.end(),
        [&](const nxsync::NextcloudEntry& entry) {
            return !entry.directory
                && entry.name == filename
                && entry.size == expectedSize;
        });
}

class NextcloudSyncTransport final : public nxsync::SyncCloudTransport {
public:
    NextcloudSyncTransport(
        nxsync::NextcloudClient& client,
        BackupUiContext& backupUi)
        : client_(client), backupUi_(backupUi) {}

    nxsync::SyncTransferResult uploadArchive(
        const std::string& localPath,
        const std::string& remotePath,
        const std::string& sha256) override {
        return convert(client_.uploadVerified(
            localPath,
            remotePath,
            sha256,
            renderNextcloudProgress,
            &backupUi_));
    }

    nxsync::SyncTransferResult uploadDocument(
        const std::string& text,
        const std::string& remotePath) override {
        return convert(client_.uploadTextVerified(
            text,
            remotePath,
            renderNextcloudProgress,
            &backupUi_));
    }

    nxsync::SyncDocumentResult downloadDocument(
        const std::string& remotePath) override {
        const nxsync::NextcloudTextResult source = client_.downloadText(remotePath);
        nxsync::SyncDocumentResult result;
        result.success = source.success;
        result.message = source.message;
        result.text = source.text;
        return result;
    }

private:
    static nxsync::SyncTransferResult convert(
        const nxsync::NextcloudResult& source) {
        return nxsync::SyncTransferResult{
            source.success,
            source.success
                ? source.message
                : nxsync::formatNextcloudFailure(source)};
    }

    nxsync::NextcloudClient& client_;
    BackupUiContext& backupUi_;
};

struct LocalStateMarkContext {
    const nxsync::DeviceIdentity* identity{nullptr};
    const nxsync::UserSaves* user{nullptr};
    const nxsync::SaveEntry* save{nullptr};
};

bool markUploadedFromSyncEngine(
    const std::string& remotePath,
    void* context,
    std::string& error) {
    const auto* mark = static_cast<const LocalStateMarkContext*>(context);
    if (mark == nullptr || mark->identity == nullptr
        || mark->user == nullptr || mark->save == nullptr) {
        error = "Invalid local status context";
        return false;
    }
    int stateError = 0;
    if (!nxsync::markLocalBackupUploaded(
            *mark->identity,
            *mark->user,
            *mark->save,
            remotePath,
            stateError)) {
        error = "Upload verified, but local status was not updated (errno "
            + std::to_string(stateError) + ")";
        return false;
    }
    error.clear();
    return true;
}

nxsync::NextcloudResult uploadBackup(
    nxsync::NextcloudClient& client,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::UserSaves& user,
    const nxsync::SaveEntry& save,
    const nxsync::LocalBackupState& state,
    BackupUiContext& backupUi) {
    if (cloudWorkerLocked()) {
        nxsync::NextcloudResult result;
        result.message = "Upload is being handled by the background cloud worker";
        return result;
    }
    const nxsync::SyncUploadPlan plan = makeSyncUploadPlan(
        config,
        identity,
        user,
        save,
        state);
    NextcloudSyncTransport transport(client, backupUi);
    LocalStateMarkContext markContext{&identity, &user, &save};
    const nxsync::SyncExecutionResult execution = nxsync::SyncEngine(
        CloudQueueRoot).execute(
            plan,
            transport,
            false,
            save.extraDataAvailable ? markUploadedFromSyncEngine : nullptr,
            save.extraDataAvailable ? &markContext : nullptr);
    nxsync::NextcloudResult result;
    result.success = execution.success;
    result.message = execution.message;
    result.remotePath = execution.remotePath;
    return result;
}

BatchBackupSummary retryPendingCloudOperations(
    nxsync::Gui& gui,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog) {
    BatchBackupSummary summary;
    const nxsync::SyncEngine engine(CloudQueueRoot);
    const std::vector<nxsync::PendingCloudOperation> pending =
        engine.pendingOperations();
    if (pending.empty()) {
        return summary;
    }
    nxsync::NextcloudClient client(config);
    if (!client.ready()) {
        summary.uploadFailed = pending.size();
        summary.firstError = client.initializationResult().message;
        for (const nxsync::PendingCloudOperation& operation : pending) {
            int queueError = 0;
            engine.recordFailure(
                operation,
                summary.firstError,
                queueError);
        }
        return summary;
    }
    std::size_t operationIndex = 0;
    for (const nxsync::PendingCloudOperation& operation : pending) {
        ++operationIndex;
        if (nxsync::launchRestoreBlocksTitle(operation.titleId)) {
            ++summary.uploadFailed;
            if (summary.firstError.empty()) summary.firstError = "Save recovery required; cloud upload is paused";
            continue;
        }
        const nxsync::UserSaves* matchedUser = nullptr;
        const nxsync::SaveEntry* matchedSave = nullptr;
        for (const nxsync::UserSaves& user : catalog.users) {
            if (formatProfileUid(user.uid) != operation.profileUid) {
                continue;
            }
            const auto save = std::find_if(
                user.saves.begin(),
                user.saves.end(),
                [&](const nxsync::SaveEntry& candidate) {
                    return nxsync::formatTitleId(candidate.applicationId) == operation.titleId
                        && nxsync::formatTitleId(candidate.saveDataId) == operation.saveDataId;
                });
            if (save != user.saves.end()) {
                matchedUser = &user;
                matchedSave = &*save;
                break;
            }
        }
        if (matchedUser == nullptr || matchedSave == nullptr) {
            ++summary.uploadFailed;
            const std::string message =
                "Queued operation has no matching profile or local save";
            if (summary.firstError.empty()) summary.firstError = message;
            int queueError = 0;
            engine.recordFailure(
                operation,
                message,
                queueError);
            continue;
        }
        const nxsync::LocalBackupState state = nxsync::findCurrentLocalBackup(
            identity,
            *matchedUser,
            *matchedSave);
        const nxsync::SyncUploadPlan plan = makeSyncUploadPlan(
            config,
            identity,
            *matchedUser,
            *matchedSave,
            state);
        if (!state.recordValid
            || engine.matchPendingOperation(operation, plan)
                != nxsync::PendingOperationMatch::Match) {
            ++summary.uploadFailed;
            const std::string message =
                "Queued operation does not match the current local backup";
            if (summary.firstError.empty()) summary.firstError = message;
            int queueError = 0;
            engine.recordFailure(
                operation,
                message,
                queueError);
            continue;
        }
        BackupUiContext backupUi{
            &gui,
            &identity,
            matchedUser,
            matchedSave,
            operationIndex,
            pending.size(),
            0};
        const nxsync::NextcloudResult upload = uploadBackup(
            client,
            config,
            identity,
            *matchedUser,
            *matchedSave,
            state,
            backupUi);
        if (upload.success) {
            ++summary.uploaded;
            ++summary.indexed;
        } else {
            ++summary.uploadFailed;
            if (upload.message.find("global index") != std::string::npos) {
                ++summary.indexFailed;
            }
            if (summary.firstError.empty()) {
                summary.firstError = upload.message;
            }
        }
    }
    return summary;
}

BatchBackupSummary createAllLocalBackups(
    nxsync::Gui& gui,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog,
    const bool reconcileRemote = true) {
    BatchBackupSummary summary;
    const nxsync::SyncEngine engine(CloudQueueRoot);
    std::unique_ptr<nxsync::NextcloudClient> nextcloud;
    bool cloudAvailable = false;
    if (config.nextcloudConfigured()) {
        nextcloud.reset(new nxsync::NextcloudClient(config));
        if (nextcloud->ready()) {
            const nxsync::NextcloudResult connection = nextcloud->testConnection();
            cloudAvailable = connection.success;
            if (!cloudAvailable) {
                summary.firstError = connection.message;
            }
        } else {
            summary.firstError = nextcloud->initializationResult().message;
        }
    }
    std::size_t batchIndex = 0;
    for (const nxsync::UserSaves& user : catalog.users) {
        for (const nxsync::SaveEntry& save : user.saves) {
            ++batchIndex;
            if (nxsync::launchRestoreBlocksTitle(nxsync::formatTitleId(save.applicationId))) {
                ++summary.failed;
                if (summary.firstError.empty()) summary.firstError = "Save recovery required; backups and retention are paused";
                continue;
            }
            BackupUiContext backupUi{
                &gui,
                &identity,
                &user,
                &save,
                batchIndex,
                catalog.totalSaves,
                0};
            nxsync::BackupProgress checkProgress;
            checkProgress.stage = "Checking save changes";
            renderBackupProgress(checkProgress, &backupUi);
            nxsync::LocalBackupState state = nxsync::findCurrentLocalBackup(
                identity,
                user,
                save);
            if (state.emptySave && !state.saveChanged) {
                ++summary.skipped;
                continue;
            } else if (state.current) {
                ++summary.skipped;
                summary.lastArchivePath = state.archivePath;
            } else {
                const nxsync::BackupResult backup = nxsync::createLocalBackup(
                    identity,
                    user,
                    save,
                    renderBackupProgress,
                    &backupUi);
                if (backup.success) {
                    ++summary.succeeded;
                    summary.lastArchivePath = backup.archivePath;
                    state = nxsync::findCurrentLocalBackup(identity, user, save);
                    if (!state.current) {
                        state.current = true;
                        state.archivePath = backup.archivePath;
                        state.sha256 = backup.sha256;
                        state.revisionId = backup.revisionId;
                        state.parentRevisionId = backup.parentRevisionId;
                        state.parentRevisionIds = backup.parentRevisionIds;
                        state.payloadSha256 = backup.payloadSha256;
                        state.fileCount = backup.fileCount;
                        state.uncompressedBytes = backup.uncompressedBytes;
                    }
                } else if (backup.emptySave) {
                    ++summary.skipped;
                    continue;
                } else {
                    ++summary.failed;
                    if (summary.firstError.empty()) {
                        const std::string title = save.titleName.empty()
                            ? nxsync::formatTitleId(save.applicationId)
                            : save.titleName;
                        summary.firstError = title + ": " + describeBackupError(backup);
                    }
                    continue;
                }
            }

            if (!config.nextcloudConfigured()) {
                mergeRetentionResult(
                    summary,
                    applyRetention(
                        config,
                        state,
                        std::string(),
                        nullptr,
                        false,
                        backupUi));
                continue;
            }
            const std::string expectedRemotePath = backupRemotePath(
                config,
                identity,
                user,
                save,
                state.archivePath);
            if (!cloudAvailable) {
                if (!state.remoteUploaded || state.remotePath != expectedRemotePath) {
                    const nxsync::SyncUploadPlan plan = makeSyncUploadPlan(
                        config,
                        identity,
                        user,
                        save,
                        state);
                    int queueError = 0;
                    engine.defer(
                        plan,
                        summary.firstError.empty()
                            ? "Nextcloud unavailable"
                            : summary.firstError,
                        queueError);
                }
                ++summary.uploadFailed;
                mergeRetentionResult(
                    summary,
                    applyRetention(
                        config,
                        state,
                        expectedRemotePath,
                        nullptr,
                        false,
                        backupUi));
                continue;
            }
            if (state.remoteUploaded && state.remotePath == expectedRemotePath) {
                if (!reconcileRemote) {
                    mergeRetentionResult(
                        summary,
                        applyRetention(
                            config,
                            state,
                            expectedRemotePath,
                            nextcloud.get(),
                            false,
                            backupUi));
                    continue;
                }
                nxsync::NextcloudProgress remoteCheck;
                remoteCheck.stage = "Checking presence on Nextcloud";
                remoteCheck.remotePath = expectedRemotePath;
                remoteCheck.totalBytes = state.archiveSize;
                renderNextcloudProgress(remoteCheck, &backupUi);
                if (remoteBackupMatches(
                        *nextcloud,
                        expectedRemotePath,
                        state.archiveSize)) {
                    const nxsync::SyncUploadPlan plan = makeSyncUploadPlan(
                        config,
                        identity,
                        user,
                        save,
                        state);
                    NextcloudSyncTransport transport(*nextcloud, backupUi);
                    LocalStateMarkContext markContext{&identity, &user, &save};
                    const nxsync::SyncExecutionResult index = engine.execute(
                        plan,
                        transport,
                        true,
                        save.extraDataAvailable ? markUploadedFromSyncEngine : nullptr,
                        save.extraDataAvailable ? &markContext : nullptr,
                        true);
                    if (!index.success) {
                        ++summary.indexFailed;
                        if (summary.firstError.empty()) {
                            summary.firstError = index.message;
                        }
                        mergeRetentionResult(
                            summary,
                            applyRetention(
                                config,
                                state,
                                expectedRemotePath,
                                nextcloud.get(),
                                false,
                                backupUi));
                        continue;
                    }
                    ++summary.indexed;
                    mergeRetentionResult(
                        summary,
                        applyRetention(
                            config,
                            state,
                            expectedRemotePath,
                            nextcloud.get(),
                            true,
                            backupUi));
                    continue;
                }
            }

            backupUi.lastUploadRenderedBytes = 0;
            const nxsync::NextcloudResult upload = uploadBackup(
                *nextcloud,
                config,
                identity,
                user,
                save,
                state,
                backupUi);
            if (upload.success) {
                ++summary.uploaded;
                ++summary.indexed;
                mergeRetentionResult(
                    summary,
                    applyRetention(
                        config,
                        state,
                        expectedRemotePath,
                        nextcloud.get(),
                        true,
                        backupUi));
            } else {
                ++summary.uploadFailed;
                if (upload.message.find("global index") != std::string::npos) {
                    ++summary.indexFailed;
                }
                if (summary.firstError.empty()) {
                    summary.firstError = upload.message;
                }
                mergeRetentionResult(
                    summary,
                    applyRetention(
                        config,
                        state,
                        expectedRemotePath,
                        nextcloud.get(),
                        false,
                        backupUi));
            }
        }
    }
    return summary;
}

std::string describeBatchSummary(
    const std::string& label,
    const BatchBackupSummary& summary,
    const bool cloudConfigured) {
    std::string message = label + ": " + std::to_string(summary.succeeded)
        + " created, " + std::to_string(summary.skipped)
        + " unchanged, " + std::to_string(summary.failed) + " local errors";
    if (cloudConfigured) {
        message += "; cloud " + std::to_string(summary.uploaded)
            + " uploaded, " + std::to_string(summary.uploadFailed) + " errors"
            + "; index " + std::to_string(summary.indexed)
            + " updated, " + std::to_string(summary.indexFailed) + " errors";
    }
    if (summary.localPruned > 0 || summary.remotePruned > 0
        || summary.retentionFailed > 0) {
        message += "; retention " + std::to_string(summary.localPruned)
            + " local, " + std::to_string(summary.remotePruned)
            + " cloud deleted, " + std::to_string(summary.retentionFailed)
            + " errors";
    }
    if (!summary.firstError.empty()) {
        message += " - first: " + summary.firstError;
    }
    return message;
}

bool showBatchBackupSummary(
    nxsync::Gui& gui,
    const nxsync::DeviceIdentity& identity,
    const nxsync::AppConfig& config,
    const BatchBackupSummary& summary,
    const std::string& operationLabel,
    const std::string& breadcrumb) {
    nxsync::GuiMenuView view;
    view.version = AppVersion;
    view.device = identity.folderName;
    view.title = operationLabel + (summary.failed == 0
            && summary.uploadFailed == 0
            && summary.indexFailed == 0
            && summary.retentionFailed == 0
        ? " completed"
        : " with errors");
    view.breadcrumb = breadcrumb;
    view.instruction = "Press A or B to open the catalog.";
    view.items.push_back(nxsync::GuiMenuItem{
        "Local backups",
        std::to_string(summary.succeeded) + " created, "
            + std::to_string(summary.skipped) + " unchanged",
        std::to_string(summary.failed) + " errors",
        summary.failed == 0 ? nxsync::GuiTone::Good : nxsync::GuiTone::Danger});
    if (config.nextcloudConfigured()) {
        view.items.push_back(nxsync::GuiMenuItem{
            "Nextcloud",
            std::to_string(summary.uploaded) + " uploaded and verified",
            std::to_string(summary.uploadFailed) + " errors",
            summary.uploadFailed == 0 ? nxsync::GuiTone::Good : nxsync::GuiTone::Danger});
        view.items.push_back(nxsync::GuiMenuItem{
            "Global index",
            std::to_string(summary.indexed) + " current revisions published",
            std::to_string(summary.indexFailed) + " errors",
            summary.indexFailed == 0 ? nxsync::GuiTone::Good : nxsync::GuiTone::Danger});
    }
    if (config.retentionCount > 0) {
        view.items.push_back(nxsync::GuiMenuItem{
            "Retention",
            std::to_string(summary.localPruned) + " local and "
                + std::to_string(summary.remotePruned) + " cloud deleted",
            std::to_string(summary.retentionFailed) + " errors",
            summary.retentionFailed == 0 ? nxsync::GuiTone::Good : nxsync::GuiTone::Danger});
    }
    if (!summary.firstError.empty()) {
        view.items.push_back(nxsync::GuiMenuItem{
            "First error",
            summary.firstError,
            "Check",
            nxsync::GuiTone::Danger});
    }
    std::size_t selected = 0;
    return chooseMenuItem(gui, view, selected) == MenuAction::Exit;
}

std::vector<std::string> listLocalDirectories(const std::string& path) {
    std::vector<std::string> names;
    DIR* handle = opendir(path.c_str());
    if (handle == nullptr) {
        return names;
    }
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string child = path + "/" + name;
        struct stat childStat{};
        if (stat(child.c_str(), &childStat) == 0 && S_ISDIR(childStat.st_mode)) {
            names.push_back(name);
        }
    }
    closedir(handle);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<LocalRestoreEntry> findLocalRestoreEntries(const std::uint64_t titleId) {
    std::vector<LocalRestoreEntry> results;
    const std::string titleFolder = nxsync::formatTitleId(titleId);
    for (const std::string& device : listLocalDirectories(LocalBackupRoot)) {
        const std::string devicePath = std::string(LocalBackupRoot) + "/" + device;
        for (const std::string& profile : listLocalDirectories(devicePath)) {
            const std::string titlePath = devicePath + "/" + profile + "/" + titleFolder;
            DIR* handle = opendir(titlePath.c_str());
            if (handle == nullptr) {
                continue;
            }
            while (dirent* entry = readdir(handle)) {
                const std::string filename = entry->d_name;
                if (!hasZipExtension(filename)) {
                    continue;
                }
                const std::string archivePath = titlePath + "/" + filename;
                struct stat archiveStat{};
                if (stat(archivePath.c_str(), &archiveStat) != 0
                    || !S_ISREG(archiveStat.st_mode)) {
                    continue;
                }
                results.push_back(LocalRestoreEntry{
                    archivePath,
                    device,
                    profile,
                    filename,
                    static_cast<std::uint64_t>(archiveStat.st_size)});
            }
            closedir(handle);
        }
    }
    std::sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        return left.filename > right.filename;
    });
    return results;
}

MenuAction showRestoreNotice(
    nxsync::Gui& gui,
    const nxsync::DeviceIdentity& identity,
    const std::string& title,
    const std::string& breadcrumb,
    const std::string& message) {
    nxsync::GuiMenuView view;
    view.version = AppVersion;
    view.device = identity.folderName;
    view.title = title;
    view.breadcrumb = breadcrumb;
    view.instruction = message + " Press B to return.";
    std::size_t selected = 0;
    return chooseMenuItem(gui, view, selected);
}

RestoreUiResult runLocalRestoreBrowser(
    nxsync::Gui& gui,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog,
    const std::uint64_t selectedTitleId,
    const std::string& selectedTitleName,
    const std::vector<LocalRestoreEntry>& localEntries) {
    RestoreUiResult uiResult;
    std::size_t selectedBackup = 0;
    while (true) {
        nxsync::GuiMenuView menu;
        menu.version = AppVersion;
        menu.device = identity.folderName;
        menu.title = "Local backups";
        menu.breadcrumb = selectedTitleName + " / "
            + nxsync::formatTitleId(selectedTitleId);
        menu.instruction = "Newest backups are shown first.";
        for (const LocalRestoreEntry& entry : localEntries) {
            const std::string date = backupDateFromArchivePath(entry.filename);
            menu.items.push_back(nxsync::GuiMenuItem{
                date.empty() ? entry.filename : "Backup " + date,
                entry.device + " / " + entry.profile + " | " + entry.filename,
                nxsync::formatByteSize(entry.size),
                nxsync::GuiTone::Accent});
        }
        const MenuAction backupAction = chooseMenuItem(gui, menu, selectedBackup);
        if (backupAction == MenuAction::Exit) {
            uiResult.exitRequested = true;
            return uiResult;
        }
        if (backupAction == MenuAction::Back) {
            uiResult.message = "Local restore cancelled";
            return uiResult;
        }
        if (selectedBackup >= localEntries.size()) {
            continue;
        }

        const LocalRestoreEntry& entry = localEntries[selectedBackup];
        gui.renderLoading(AppVersion, "Verifying local backup", "Checking manifest, paths and CRC...");
        const nxsync::RestoreInspection inspection =
            nxsync::inspectRestoreArchive(entry.archivePath);
        if (!inspection.success) {
            const MenuAction action = showRestoreNotice(
                gui,
                identity,
                "Local backup cannot be restored",
                entry.filename,
                inspection.message + ".");
            if (action == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }
        if (inspection.manifest.titleId != selectedTitleId) {
            const MenuAction action = showRestoreNotice(
                gui,
                identity,
                "Title ID mismatch",
                entry.filename,
                "The manifest does not belong to the selected game.");
            if (action == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }

        const nxsync::ApplicationSaveDataDefaults metadata =
            nxsync::loadApplicationSaveDataDefaults(selectedTitleId);
        if (!metadata.available) {
            const MenuAction action = showRestoreNotice(
                gui,
                identity,
                "Exact game is not installed",
                nxsync::formatTitleId(selectedTitleId),
                "NXSync requires the same Title ID as the backup.");
            if (action == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }
        const nxsync::GameVersionOrder versionOrder = nxsync::compareGameVersions(
            metadata.gameVersion,
            inspection.manifest.gameVersion);
        if (versionOrder == nxsync::GameVersionOrder::Older) {
            const MenuAction action = showRestoreNotice(
                gui,
                identity,
                "Game version is too old",
                "Backup " + inspection.manifest.gameVersion
                    + " / installed " + metadata.gameVersion,
                "Update the game to at least the backup version.");
            if (action == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }

        nxsync::GuiMenuView targetMenu;
        targetMenu.version = AppVersion;
        targetMenu.device = identity.folderName;
        targetMenu.title = "Choose the destination profile";
        targetMenu.breadcrumb = inspection.manifest.sourceDevice + " / "
            + inspection.manifest.sourceProfile + " / " + selectedTitleName;
        targetMenu.instruction = inspection.manifest.gameVersion.empty()
            ? "Source version was not recorded: compatibility cannot be verified."
            : "Backup " + inspection.manifest.gameVersion + " | Installed "
                + (metadata.gameVersion.empty() ? "unknown" : metadata.gameVersion);
        for (const nxsync::UserSaves& user : catalog.users) {
            const bool hasSave = std::any_of(
                user.saves.begin(),
                user.saves.end(),
                [&](const nxsync::SaveEntry& save) {
                    return save.applicationId == selectedTitleId;
                });
            targetMenu.items.push_back(nxsync::GuiMenuItem{
                user.nickname,
                hasSave
                    ? "Existing container; its data will be protected"
                    : "A new container will be created",
                hasSave ? "Existing" : "New"});
        }
        std::size_t targetIndex = 0;
        const MenuAction targetAction = chooseMenuItem(gui, targetMenu, targetIndex);
        if (targetAction == MenuAction::Exit) {
            uiResult.exitRequested = true;
            return uiResult;
        }
        if (targetAction == MenuAction::Back || targetIndex >= catalog.users.size()) {
            continue;
        }

        bool confirmationExit = false;
        if (!confirmRestore(
                gui,
                identity.folderName,
                inspection,
                catalog.users[targetIndex],
                confirmationExit)) {
            if (confirmationExit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }

        nxsync::SaveEntry displaySave;
        displaySave.applicationId = selectedTitleId;
        displaySave.titleName = selectedTitleName;
        BackupUiContext restoreUi{
            &gui,
            &identity,
            &catalog.users[targetIndex],
            &displaySave,
            0,
            0,
            0};
        const nxsync::RestoreResult restore = nxsync::restoreArchiveToProfile(
            entry.archivePath,
            inspection,
            identity,
            catalog.users[targetIndex],
            renderBackupProgress,
            &restoreUi);
        int guardError = 0;
        if (restore.success && !cloudWorkerLocked()) {
            nxsync::clearRecoveredLaunchGuard(
                nxsync::formatTitleId(inspection.manifest.titleId),
                formatProfileUid(catalog.users[targetIndex].uid), guardError);
        }
        uiResult.catalogChanged = true;
        uiResult.success = restore.success;
        uiResult.message = restore.message;
        if (guardError != 0 && guardError != EBUSY)
            uiResult.message += "; launch recovery marker could not be cleared";
        uiResult.safetyBackupPath = restore.safetyBackupPath;
        if (R_FAILED(restore.systemResult)) {
            uiResult.message += " (" + nxsync::formatResult(restore.systemResult) + ")";
        } else if (restore.systemError != 0) {
            uiResult.message += " (errno " + std::to_string(restore.systemError) + ")";
        }

        nxsync::GuiMenuView resultView;
        resultView.version = AppVersion;
        resultView.device = identity.folderName;
        resultView.title = restore.success
            ? "Local restore completed"
            : "Local restore was not completed";
        resultView.breadcrumb = selectedTitleName;
        resultView.instruction = "Press A or B to return to the catalog.";
        resultView.items.push_back(nxsync::GuiMenuItem{
            restore.success
                ? "Save written and confirmed"
                : "No restore confirmed",
            uiResult.message,
            restore.success
                ? std::to_string(restore.restoredFiles) + " file | "
                    + nxsync::formatByteSize(restore.restoredBytes)
                : "Error",
            restore.success ? nxsync::GuiTone::Good : nxsync::GuiTone::Danger});
        resultView.items.push_back(nxsync::GuiMenuItem{
            "Local archive retained",
            entry.device + " / " + entry.profile + " / " + entry.filename,
            "microSD",
            nxsync::GuiTone::Neutral});
        if (restore.recoveryAttempted) {
            resultView.items.push_back(nxsync::GuiMenuItem{
                restore.recoverySucceeded
                    ? "Automatic recovery completed"
                    : "Automatic recovery failed",
                restore.recoverySucceeded
                    ? "The previous destination was protected"
                    : "Keep the safety backup and do not launch the game",
                restore.recoverySucceeded ? "Protected" : "Warning",
                restore.recoverySucceeded
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Danger});
        }
        std::size_t resultSelection = 0;
        if (chooseMenuItem(gui, resultView, resultSelection) == MenuAction::Exit) {
            uiResult.exitRequested = true;
        }
        return uiResult;
    }
}

RestoreUiResult runCloudRestoreBrowser(
    nxsync::Gui& gui,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog) {
    RestoreUiResult uiResult;
    if (!config.nextcloudConfigured()) {
        uiResult.message = "Configure Nextcloud before opening Restore";
        return uiResult;
    }
    if (catalog.users.empty()) {
        uiResult.message = "No local profile is available as a destination";
        return uiResult;
    }

    gui.renderLoading(AppVersion, "Opening Nextcloud backups", "Connecting to WebDAV...");
    nxsync::NextcloudClient client(config);
    if (!client.ready()) {
        uiResult.message = client.initializationResult().message;
        return uiResult;
    }

    const std::string rootPath = nxsync::normalizeRemoteRoot(config.remoteRoot);
    std::vector<std::string> paths{rootPath};
    std::vector<std::string> labels;
    std::array<std::size_t, 4> selectedByLevel{};
    std::uint64_t selectedTitleId = 0;
    std::map<std::uint64_t, nxsync::ApplicationSaveDataDefaults> titleMetadataCache;

    while (true) {
        const std::size_t level = paths.size() - 1;
        const char* levelNames[] = {
            "Select the source Switch",
            "Select the source profile",
            "Select the game",
            "Select the backup",
        };
        gui.renderLoading(
            AppVersion,
            levelNames[std::min<std::size_t>(level, 3)],
            "Reading Nextcloud folder...");
        nxsync::NextcloudListResult listing = client.listDirectory(paths.back());
        if (!listing.success) {
            uiResult.message = listing.message;
            if (listing.httpStatus != 0) {
                uiResult.message += " (HTTP " + std::to_string(listing.httpStatus) + ")";
            }
            return uiResult;
        }

        std::vector<nxsync::NextcloudEntry> choices;
        for (const nxsync::NextcloudEntry& entry : listing.entries) {
            if (level == 0 && entry.directory && entry.name == "_index") {
                continue;
            }
            if (level < 3 && !entry.directory) {
                continue;
            }
            if (level == 2) {
                std::uint64_t ignored = 0;
                if (!parseTitleIdFolder(entry.name, ignored)) {
                    continue;
                }
            }
            if (level == 3 && (entry.directory || !hasZipExtension(entry.name))) {
                continue;
            }
            choices.push_back(entry);
        }
        if (level == 3) {
            std::sort(choices.begin(), choices.end(), [](const auto& left, const auto& right) {
                return left.name > right.name;
            });
        }

        nxsync::GuiMenuView menu;
        menu.version = AppVersion;
        menu.device = identity.folderName;
        menu.title = levelNames[std::min<std::size_t>(level, 3)];
        menu.breadcrumb = rootPath;
        for (const std::string& label : labels) {
            menu.breadcrumb += " / " + label;
        }
        menu.instruction = level == 3
            ? "Newest backups are shown first."
            : "A selects, B returns to the previous level.";
        std::vector<std::string> choiceLabels;
        choiceLabels.reserve(choices.size());
        for (const nxsync::NextcloudEntry& entry : choices) {
            nxsync::GuiMenuItem item;
            item.primary = entry.name;
            item.secondary = entry.modified;
            item.badge = entry.directory ? "Folder" : nxsync::formatByteSize(entry.size);
            if (level == 0) {
                item.secondary = "Source console";
                item.badge = "Switch";
            } else if (level == 1) {
                item.secondary = "Source profile";
                item.badge = "Profile";
            } else if (level == 3) {
                const std::string backupDate = backupDateFromArchivePath(entry.name);
                if (!backupDate.empty()) {
                    item.primary = "Backup " + backupDate;
                    item.secondary = entry.name;
                    if (!entry.modified.empty()) {
                        item.secondary += " | Server: " + entry.modified;
                    }
                }
            }
            if (level == 2) {
                std::uint64_t titleId = 0;
                parseTitleIdFolder(entry.name, titleId);
                auto metadata = titleMetadataCache.find(titleId);
                if (metadata == titleMetadataCache.end()) {
                    gui.renderLoading(
                        AppVersion,
                        "Resolving game names",
                        "Reading metadata " + entry.name + "...");
                    metadata = titleMetadataCache.emplace(
                        titleId,
                        nxsync::loadApplicationSaveDataDefaults(titleId)).first;
                }
                item.primary = metadata->second.titleName.empty()
                    ? "Game not installed / name unavailable"
                    : metadata->second.titleName;
                item.secondary = entry.name;
                item.badge = metadata->second.available ? "Installed" : "Not installed";
                item.tone = metadata->second.available
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Danger;
            }
            choiceLabels.push_back(item.primary);
            menu.items.push_back(std::move(item));
        }

        std::size_t& selected = selectedByLevel[std::min<std::size_t>(level, 3)];
        const MenuAction action = chooseMenuItem(gui, menu, selected);
        if (action == MenuAction::Exit) {
            uiResult.exitRequested = true;
            return uiResult;
        }
        if (action == MenuAction::Back) {
            if (level == 0) {
                uiResult.message = "Restore cancelled";
                return uiResult;
            }
            paths.pop_back();
            labels.pop_back();
            continue;
        }
        if (choices.empty() || selected >= choices.size()) {
            continue;
        }

        const nxsync::NextcloudEntry chosen = choices[selected];
        if (level < 3) {
            if (level == 2) {
                parseTitleIdFolder(chosen.name, selectedTitleId);
                const auto metadata = titleMetadataCache.find(selectedTitleId);
                if (metadata == titleMetadataCache.end()
                    || !metadata->second.available) {
                    nxsync::GuiMenuView unavailable;
                    unavailable.version = AppVersion;
                    unavailable.device = identity.folderName;
                    unavailable.title = "Exact game is not installed";
                    unavailable.breadcrumb = chosen.name;
                    unavailable.instruction =
                        "NXSync requires the same Title ID as the backup. "
                        "Different editions, such as Pokemon Scarlet and Violet, "
                        "are not automatically interchangeable. Press B to return.";
                    std::size_t ignored = 0;
                    const MenuAction unavailableAction = chooseMenuItem(
                        gui,
                        unavailable,
                        ignored);
                    if (unavailableAction == MenuAction::Exit) {
                        uiResult.exitRequested = true;
                        return uiResult;
                    }
                    continue;
                }
            }
            paths.push_back(chosen.remotePath);
            labels.push_back(
                selected < choiceLabels.size() ? choiceLabels[selected] : chosen.name);
            continue;
        }

        mkdir("sdmc:/switch/NXSync", 0777);
        mkdir("sdmc:/switch/NXSync/imports", 0777);
        const std::string localDownload = "sdmc:/switch/NXSync/imports/restore-download.zip";
        BackupUiContext downloadUi{&gui, &identity, nullptr, nullptr, 0, 0, 0};
        const nxsync::NextcloudResult download = client.downloadVerified(
            chosen.remotePath,
            localDownload,
            renderNextcloudProgress,
            &downloadUi);
        if (!download.success) {
            uiResult.message = download.message;
            return uiResult;
        }

        gui.renderLoading(AppVersion, "Verifying backup", "Checking manifest, paths and CRC...");
        const nxsync::RestoreInspection inspection = nxsync::inspectRestoreArchive(localDownload);
        if (!inspection.success) {
            nxsync::GuiMenuView invalidBackup;
            invalidBackup.version = AppVersion;
            invalidBackup.device = identity.folderName;
            invalidBackup.title = "Backup cannot be restored";
            invalidBackup.breadcrumb = chosen.name;
            invalidBackup.instruction = inspection.message
                + ". Press B to choose another backup.";
            std::size_t ignored = 0;
            const MenuAction invalidAction = chooseMenuItem(
                gui,
                invalidBackup,
                ignored);
            if (invalidAction == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }
        if (inspection.manifest.titleId != selectedTitleId) {
            nxsync::GuiMenuView invalidBackup;
            invalidBackup.version = AppVersion;
            invalidBackup.device = identity.folderName;
            invalidBackup.title = "Backup cannot be restored";
            invalidBackup.breadcrumb = chosen.name;
            invalidBackup.instruction =
                "Manifest Title ID differs from the Nextcloud folder. "
                "Press B to choose another backup.";
            std::size_t ignored = 0;
            const MenuAction invalidAction = chooseMenuItem(
                gui,
                invalidBackup,
                ignored);
            if (invalidAction == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }

        const auto selectedMetadata = titleMetadataCache.find(selectedTitleId);
        const std::string installedVersion = selectedMetadata == titleMetadataCache.end()
            ? std::string()
            : selectedMetadata->second.gameVersion;
        const nxsync::GameVersionOrder versionOrder = nxsync::compareGameVersions(
            installedVersion,
            inspection.manifest.gameVersion);
        if (versionOrder == nxsync::GameVersionOrder::Older) {
            nxsync::GuiMenuView incompatibleVersion;
            incompatibleVersion.version = AppVersion;
            incompatibleVersion.device = identity.folderName;
            incompatibleVersion.title = "Game version is too old";
            incompatibleVersion.breadcrumb = "Backup " + inspection.manifest.gameVersion
                + " / installed " + installedVersion;
            incompatibleVersion.instruction =
                "Update the game to at least the backup version before restoring. "
                "Press B to choose another backup.";
            std::size_t ignored = 0;
            const MenuAction incompatibleAction = chooseMenuItem(
                gui,
                incompatibleVersion,
                ignored);
            if (incompatibleAction == MenuAction::Exit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }

        nxsync::GuiMenuView targetMenu;
        targetMenu.version = AppVersion;
        targetMenu.device = identity.folderName;
        targetMenu.title = "Choose the destination profile";
        targetMenu.breadcrumb = inspection.manifest.sourceDevice + " / "
            + inspection.manifest.sourceProfile + " / "
            + (inspection.manifest.titleName.empty()
                ? nxsync::formatTitleId(inspection.manifest.titleId)
                : inspection.manifest.titleName);
        if (inspection.manifest.gameVersion.empty()) {
            targetMenu.instruction =
                "Source version was not recorded: compatibility cannot be verified. "
                "The source UID will not be copied; internal game data will not be adapted.";
        } else if (versionOrder == nxsync::GameVersionOrder::Unknown) {
            targetMenu.instruction = "Versions cannot be compared (backup "
                + inspection.manifest.gameVersion + ", installed "
                + (installedVersion.empty() ? "unknown" : installedVersion)
                + "). Internal data, DLC and licenses are not adapted.";
        } else {
            targetMenu.instruction = "Compatible version: backup "
                + inspection.manifest.gameVersion + ", installed "
                + installedVersion + ". DLC, licenses and internal data remain game-dependent.";
        }
        for (const nxsync::UserSaves& user : catalog.users) {
            const bool hasSave = std::any_of(
                user.saves.begin(),
                user.saves.end(),
                [&](const nxsync::SaveEntry& save) {
                    return save.applicationId == inspection.manifest.titleId;
                });
            targetMenu.items.push_back(nxsync::GuiMenuItem{
                user.nickname,
                hasSave
                    ? "Existing container; its data will be protected"
                    : "A new container will be created",
                hasSave ? "Existing" : "New"});
        }
        std::size_t targetIndex = 0;
        const MenuAction targetAction = chooseMenuItem(gui, targetMenu, targetIndex);
        if (targetAction == MenuAction::Exit) {
            uiResult.exitRequested = true;
            return uiResult;
        }
        if (targetAction == MenuAction::Back) {
            continue;
        }
        if (targetIndex >= catalog.users.size()) {
            continue;
        }

        bool confirmationExit = false;
        if (!confirmRestore(
                gui,
                identity.folderName,
                inspection,
                catalog.users[targetIndex],
                confirmationExit)) {
            if (confirmationExit) {
                uiResult.exitRequested = true;
                return uiResult;
            }
            continue;
        }

        nxsync::SaveEntry displaySave;
        displaySave.applicationId = inspection.manifest.titleId;
        displaySave.titleName = inspection.manifest.titleName;
        BackupUiContext restoreUi{
            &gui,
            &identity,
            &catalog.users[targetIndex],
            &displaySave,
            0,
            0,
            0};
        const nxsync::RestoreResult restore = nxsync::restoreArchiveToProfile(
            localDownload,
            inspection,
            identity,
            catalog.users[targetIndex],
            renderBackupProgress,
            &restoreUi);
        int guardError = 0;
        if (restore.success && !cloudWorkerLocked()) {
            nxsync::clearRecoveredLaunchGuard(
                nxsync::formatTitleId(inspection.manifest.titleId),
                formatProfileUid(catalog.users[targetIndex].uid), guardError);
        }
        uiResult.catalogChanged = true;
        uiResult.success = restore.success;
        uiResult.message = restore.message;
        if (guardError != 0 && guardError != EBUSY)
            uiResult.message += "; launch recovery marker could not be cleared";
        uiResult.safetyBackupPath = restore.safetyBackupPath;
        if (R_FAILED(restore.systemResult)) {
            uiResult.message += " (" + nxsync::formatResult(restore.systemResult) + ")";
        } else if (restore.systemError != 0) {
            uiResult.message += " (errno " + std::to_string(restore.systemError) + ")";
        }
        if (restore.success) {
            std::remove(localDownload.c_str());
        }

        nxsync::GuiMenuView resultView;
        resultView.version = AppVersion;
        resultView.device = identity.folderName;
        resultView.title = restore.success
            ? "Restore completed"
            : "Restore was not completed";
        resultView.breadcrumb = inspection.manifest.titleName.empty()
            ? nxsync::formatTitleId(inspection.manifest.titleId)
            : inspection.manifest.titleName;
        resultView.instruction = "Press A or B to return to the catalog.";
        resultView.items.push_back(nxsync::GuiMenuItem{
            restore.success
                ? "Save written and confirmed"
                : "No restore confirmed",
            uiResult.message,
            restore.success
                ? std::to_string(restore.restoredFiles) + " file | "
                    + nxsync::formatByteSize(restore.restoredBytes)
                : "Error",
            restore.success ? nxsync::GuiTone::Good : nxsync::GuiTone::Danger});
        if (restore.success) {
            resultView.items.push_back(nxsync::GuiMenuItem{
                "Source",
                (inspection.manifest.sourceDevice.empty()
                    ? std::string("Unknown console")
                    : inspection.manifest.sourceDevice)
                    + " / "
                    + (inspection.manifest.sourceProfile.empty()
                        ? std::string("Unknown profile")
                        : inspection.manifest.sourceProfile)
                    + (backupDateFromArchivePath(inspection.manifest.createdUtc).empty()
                        ? std::string()
                        : " | " + backupDateFromArchivePath(inspection.manifest.createdUtc)),
                "Origin",
                nxsync::GuiTone::Neutral});
            resultView.items.push_back(nxsync::GuiMenuItem{
                "Game versions",
                "Backup "
                    + (inspection.manifest.gameVersion.empty()
                        ? std::string("not recorded")
                        : inspection.manifest.gameVersion)
                    + " | Installed "
                    + (installedVersion.empty()
                        ? std::string("unknown")
                        : installedVersion),
                "Verified",
                versionOrder == nxsync::GameVersionOrder::Equal
                        || versionOrder == nxsync::GameVersionOrder::Newer
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Warning});
        }
        if (restore.recoveryAttempted) {
            resultView.items.push_back(nxsync::GuiMenuItem{
                restore.recoverySucceeded
                    ? "Automatic recovery completed"
                    : "Automatic recovery failed",
                restore.safetyBackupRestored
                    ? "The previous local save was restored from the safety backup"
                    : (restore.recoverySucceeded
                        ? (restore.createdContainer
                            ? "The container created for import was removed"
                            : "The destination had no previous data and was left empty")
                        : "Keep the safety backup and do not launch the game"),
                restore.recoverySucceeded ? "Protected" : "Warning",
                restore.recoverySucceeded
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Danger});
        }
        if (restore.success) {
            resultView.items.push_back(nxsync::GuiMenuItem{
                "Compatibility handled by the game",
                "NXSync does not modify DLC, licenses, online accounts or internal identifiers",
                "Note",
                nxsync::GuiTone::Warning});
        }
        std::size_t resultSelection = 0;
        const MenuAction resultAction = chooseMenuItem(gui, resultView, resultSelection);
        if (resultAction == MenuAction::Exit) {
            uiResult.exitRequested = true;
        }
        return uiResult;
    }
}

MenuAction showCloudRevisionStatus(
    nxsync::Gui& gui,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::UserSaves& selectedUser,
    const nxsync::SaveEntry& selectedSave) {
    gui.renderLoading(
        AppVersion,
        "Comparing revisions",
        "Reading global index " + nxsync::formatTitleId(selectedSave.applicationId) + "...");
    nxsync::NextcloudClient client(config);
    if (!client.ready()) {
        return showRestoreNotice(
            gui,
            identity,
            "Global index unavailable",
            nxsync::formatTitleId(selectedSave.applicationId),
            client.initializationResult().message + ".");
    }

    const std::string titleId = nxsync::formatTitleId(selectedSave.applicationId);
    const std::string titleRoot = nxsync::normalizeRemoteRoot(config.remoteRoot)
        + "/_index/titles/" + titleId;
    const nxsync::NextcloudListResult headListing = client.listDirectory(titleRoot);
    if (!headListing.success) {
        return showRestoreNotice(
            gui,
            identity,
            "No cloud revision",
            titleId,
            "The game's global index is not available yet.");
    }

    std::vector<nxsync::CloudIndexEntry> heads;
    for (const nxsync::NextcloudEntry& remote : headListing.entries) {
        if (remote.directory || remote.name.size() < 6
            || remote.name.substr(remote.name.size() - 5) != ".json") {
            continue;
        }
        const nxsync::NextcloudTextResult downloaded = client.downloadText(remote.remotePath);
        nxsync::CloudIndexEntry entry;
        std::string error;
        if (downloaded.success
            && nxsync::parseCloudIndexEntry(downloaded.text, entry, error)
            && entry.titleId == titleId) {
            heads.push_back(std::move(entry));
        }
    }

    std::vector<nxsync::RevisionNode> history;
    const std::string revisionRoot = nxsync::normalizeRemoteRoot(config.remoteRoot)
        + "/_index/revisions/" + titleId;
    const nxsync::NextcloudListResult revisionListing = client.listDirectory(revisionRoot);
    if (revisionListing.success) {
        for (const nxsync::NextcloudEntry& remote : revisionListing.entries) {
            if (remote.directory || remote.name.size() < 6
                || remote.name.substr(remote.name.size() - 5) != ".json") {
                continue;
            }
            const nxsync::NextcloudTextResult downloaded = client.downloadText(remote.remotePath);
            nxsync::CloudIndexEntry entry;
            std::string error;
            if (downloaded.success
                && nxsync::parseCloudIndexEntry(downloaded.text, entry, error)
                && entry.titleId == titleId) {
                history.push_back(nxsync::RevisionNode{
                    entry.revisionId,
                    entry.parentRevisionId,
                    entry.payloadSha256,
                    entry.parentRevisionIds});
            }
        }
    }

    const nxsync::LocalBackupState local = nxsync::findCurrentLocalBackup(
        identity,
        selectedUser,
        selectedSave);
    const nxsync::RestoreLineageAnchor restoreAnchor =
        nxsync::loadRestoreLineageAnchor(
            identity,
            selectedUser,
            selectedSave.applicationId);
    const nxsync::RevisionNode localNode = local.current
        ? nxsync::RevisionNode{
            local.revisionId,
            local.parentRevisionId,
            local.payloadSha256,
            local.parentRevisionIds}
        : nxsync::restoredRevisionNode(restoreAnchor);

    nxsync::GuiMenuView menu;
    menu.version = AppVersion;
    menu.device = identity.folderName;
    menu.title = "Synchronization status";
    menu.breadcrumb = (selectedSave.titleName.empty() ? titleId : selectedSave.titleName)
        + " / " + titleId;
    menu.instruction = "Comparison with the local backup for profile "
        + selectedUser.nickname + ". Press B to return.";
    for (const nxsync::CloudIndexEntry& head : heads) {
        const nxsync::RevisionRelation relation = nxsync::compareRevisions(
            localNode,
            nxsync::RevisionNode{
                head.revisionId,
                head.parentRevisionId,
                head.payloadSha256,
                head.parentRevisionIds},
            history);
        nxsync::GuiTone tone = nxsync::GuiTone::Neutral;
        if (relation == nxsync::RevisionRelation::SameRevision
            || relation == nxsync::RevisionRelation::SamePayload) {
            tone = nxsync::GuiTone::Good;
        } else if (relation == nxsync::RevisionRelation::Diverged
            || relation == nxsync::RevisionRelation::Unrelated
            || relation == nxsync::RevisionRelation::Invalid) {
            tone = nxsync::GuiTone::Danger;
        } else if (relation == nxsync::RevisionRelation::CloudNewer
            || relation == nxsync::RevisionRelation::NoLocalRevision) {
            tone = nxsync::GuiTone::Warning;
        } else {
            tone = nxsync::GuiTone::Accent;
        }
        menu.items.push_back(nxsync::GuiMenuItem{
            head.deviceId + " / " + head.profileName,
            "Backup " + backupDateFromArchivePath(head.createdUtc)
                + " | Game " + (head.gameVersion.empty() ? "?" : head.gameVersion)
                + " | " + nxsync::formatByteSize(head.uncompressedBytes),
            nxsync::revisionRelationLabel(relation),
            tone});
    }
    if (menu.items.empty()) {
        menu.instruction = "No valid head found for this Title ID. Press B to return.";
    }
    std::size_t selected = 0;
    return chooseMenuItem(gui, menu, selected);
}

RestoreUiResult runRestoreBrowser(
    nxsync::Gui& gui,
    const nxsync::AppConfig& config,
    const nxsync::DeviceIdentity& identity,
    const nxsync::SaveCatalog& catalog,
    const nxsync::UserSaves& selectedUser,
    const nxsync::SaveEntry& selectedSave) {
    RestoreUiResult uiResult;
    const std::uint64_t selectedTitleId = selectedSave.applicationId;
    const std::string selectedTitleName = selectedSave.titleName.empty()
        ? "Title " + nxsync::formatTitleId(selectedTitleId)
        : selectedSave.titleName;
    if (catalog.users.empty()) {
        uiResult.message = "No local profile is available as a destination";
        return uiResult;
    }

    gui.renderLoading(
        AppVersion,
        "Searching backups",
        "Checking local archives for " + nxsync::formatTitleId(selectedTitleId) + "...");
    const std::vector<LocalRestoreEntry> localEntries =
        findLocalRestoreEntries(selectedTitleId);

    nxsync::GuiMenuView sourceMenu;
    sourceMenu.version = AppVersion;
    sourceMenu.device = identity.folderName;
    sourceMenu.title = "Backup source";
    sourceMenu.breadcrumb = selectedTitleName + " / "
        + nxsync::formatTitleId(selectedTitleId);
    sourceMenu.instruction = "Choose microSD or Nextcloud. B returns to the catalog.";
    enum class RestoreSource { Status, Local, Cloud };
    std::vector<RestoreSource> sources;
    if (config.nextcloudConfigured()) {
        sources.push_back(RestoreSource::Status);
        sourceMenu.items.push_back(nxsync::GuiMenuItem{
            "Synchronization status",
            "Compare the local backup with every console and profile",
            "Global index",
            nxsync::GuiTone::Accent});
    }
    if (!localEntries.empty()) {
        sources.push_back(RestoreSource::Local);
        sourceMenu.items.push_back(nxsync::GuiMenuItem{
            "Local backups",
            "Archives for this game found on the microSD card",
            std::to_string(localEntries.size()) + " available",
            nxsync::GuiTone::Good});
    }
    if (config.nextcloudConfigured()) {
        sources.push_back(RestoreSource::Cloud);
        sourceMenu.items.push_back(nxsync::GuiMenuItem{
            "Nextcloud",
            "Browse consoles, profiles, games and cloud backups",
            "WebDAV",
            nxsync::GuiTone::Accent});
    }
    if (sources.empty()) {
        uiResult.message =
            "No local backup for this game and Nextcloud is not configured";
        return uiResult;
    }

    std::size_t sourceIndex = 0;
    const MenuAction sourceAction = chooseMenuItem(gui, sourceMenu, sourceIndex);
    if (sourceAction == MenuAction::Exit) {
        uiResult.exitRequested = true;
        return uiResult;
    }
    if (sourceAction == MenuAction::Back || sourceIndex >= sources.size()) {
        uiResult.message = "Restore cancelled";
        return uiResult;
    }
    if (sources[sourceIndex] == RestoreSource::Local) {
        return runLocalRestoreBrowser(
            gui,
            identity,
            catalog,
            selectedTitleId,
            selectedTitleName,
            localEntries);
    }
    if (sources[sourceIndex] == RestoreSource::Status) {
        const MenuAction statusAction = showCloudRevisionStatus(
            gui,
            config,
            identity,
            selectedUser,
            selectedSave);
        if (statusAction == MenuAction::Exit) {
            uiResult.exitRequested = true;
        } else {
            uiResult.message = "Revision comparison completed";
        }
        return uiResult;
    }
    return runCloudRestoreBrowser(gui, config, identity, catalog);
}

void renderCatalog(
    nxsync::Gui& gui,
    const nxsync::DeviceIdentity& identity,
    const nxsync::AppConfig& config,
    const std::string& remoteRoot,
    const nxsync::SaveCatalog& catalog,
    const BackupStateCache& backupStates,
    const std::size_t userIndex,
    const std::size_t pageIndex,
    const std::size_t selectedSaveIndex,
    const std::string& lastMessage,
    const std::string& lastArchivePath,
    const nxsync::GuiTone lastMessageTone,
    const bool batchConfirmationPending) {
    nxsync::GuiCatalogView view;
    view.version = AppVersion;
    view.device = identity.folderName;
    view.remoteRoot = remoteRoot;
    view.nextcloudConfigured = config.nextcloudConfigured();
    view.pendingCloudOperations =
        nxsync::SyncEngine(CloudQueueRoot).pendingOperations().size();
    view.autoBackupOnStart = config.autoBackupOnStart;
    view.retentionCount = config.retentionCount;
    view.statusMessage = lastMessage;
    view.statusPath = lastArchivePath;
    view.statusTone = lastMessageTone;
    view.batchConfirmationPending = batchConfirmationPending;
    view.batchTotal = catalog.totalSaves;

    if (!catalog.ok()) {
        view.profileName = "Unavailable";
        view.emptyMessage = "Unable to read saves";
        view.note = "FS result: " + nxsync::formatResult(catalog.saveReaderResult);
        gui.renderCatalog(view);
        return;
    }

    if (catalog.users.empty()) {
        view.profileName = "No profile";
        view.emptyMessage = "No user profile or save found";
        if (R_FAILED(catalog.accountResult)) {
            view.note = "Account service: " + nxsync::formatResult(catalog.accountResult);
        }
        gui.renderCatalog(view);
        return;
    }

    const nxsync::UserSaves& user = catalog.users[userIndex];
    const std::size_t totalPages = pageCount(user);
    const std::size_t firstEntry = pageIndex * SavesPerPage;
    const std::size_t lastEntry = std::min(
        firstEntry + SavesPerPage,
        user.saves.size());

    view.profileName = user.nickname;
    view.profileLabel = "Profile " + std::to_string(userIndex + 1)
        + "/" + std::to_string(catalog.registeredProfiles);
    view.saveSummary = std::to_string(user.saves.size())
        + " saves - " + std::to_string(catalog.totalSaves) + " total";
    view.pageLabel = std::to_string(pageIndex + 1) + "/" + std::to_string(totalPages);

    if (R_FAILED(catalog.accountResult)) {
        view.note = "Account unavailable: "
            + nxsync::formatResult(catalog.accountResult);
    }

    if (user.saves.empty()) {
        view.emptyMessage = "This profile has no Account/User saves";
    } else {
        for (std::size_t index = firstEntry; index < lastEntry; ++index) {
            const nxsync::SaveEntry& save = user.saves[index];
            const std::string displayName = save.titleName.empty()
                ? "Title " + nxsync::formatTitleId(save.applicationId)
                : save.titleName;

            nxsync::LocalBackupState backupState;
            if (userIndex < backupStates.size()
                && index < backupStates[userIndex].size()) {
                backupState = backupStates[userIndex][index];
            }
            const nxsync::RestoreLineageAnchor restoreAnchor =
                nxsync::loadRestoreLineageAnchor(
                    identity,
                    user,
                    save.applicationId);
            nxsync::GuiSaveState guiState = nxsync::GuiSaveState::Unknown;
            std::string backupStatus = "Status cannot be verified";
            if (!save.extraDataAvailable) {
                guiState = nxsync::GuiSaveState::Unknown;
            } else if (restoreAnchor.valid) {
                guiState = nxsync::GuiSaveState::Changed;
                backupStatus = "Restored from cloud - continuity recorded";
            } else if (!backupState.indexed) {
                guiState = nxsync::GuiSaveState::NotBackedUp;
                backupStatus = "Never backed up";
            } else if (backupState.emptySave && backupState.saveChanged) {
                guiState = nxsync::GuiSaveState::Changed;
                backupStatus = "Changed - ready for the first backup";
            } else if (backupState.emptySave) {
                guiState = nxsync::GuiSaveState::NotBackedUp;
                backupStatus = "No save data for this profile";
            } else if (!backupState.archivePresent) {
                guiState = nxsync::GuiSaveState::Error;
                backupStatus = "Error: local ZIP is missing";
            } else if (backupState.legacy) {
                guiState = nxsync::GuiSaveState::Changed;
                backupStatus = "Legacy backup - create a new v2 backup";
            } else if (!backupState.recordValid) {
                guiState = nxsync::GuiSaveState::Error;
                backupStatus = statusWithBackupDate("Error: invalid index", backupState);
            } else if (backupState.saveChanged) {
                guiState = nxsync::GuiSaveState::Changed;
                backupStatus = statusWithBackupDate("Changed", backupState);
            } else if (config.nextcloudConfigured()) {
                const std::string expectedRemotePath = backupRemotePath(
                    config,
                    identity,
                    user,
                    save,
                    backupState.archivePath);
                if (backupState.remoteUploaded
                    && backupState.remotePath == expectedRemotePath) {
                    guiState = nxsync::GuiSaveState::CloudCurrent;
                    backupStatus = statusWithBackupDate("Cloud up to date", backupState);
                } else {
                    guiState = nxsync::GuiSaveState::UploadPending;
                    backupStatus = statusWithBackupDate("Upload pending", backupState);
                }
            } else {
                guiState = nxsync::GuiSaveState::LocalCurrent;
                backupStatus = statusWithBackupDate("Local backup up to date", backupState);
            }
            std::string metadata = nxsync::formatTitleId(save.applicationId)
                + " | " + nxsync::formatByteSize(save.rawSize);
            if (backupState.archivePresent) {
                metadata += " | ZIP " + nxsync::formatByteSize(backupState.archiveSize);
            }
            view.rows.push_back(nxsync::GuiSaveRow{
                index == selectedSaveIndex,
                displayName,
                metadata,
                backupStatus,
                guiState});
        }
    }
    if (!catalog.titleServiceAvailable) {
        view.note = "Title names unavailable: "
            + nxsync::formatResult(catalog.titleServiceResult);
    }
    gui.renderCatalog(view);
}

} // namespace

int main(int, char**) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    nxsync::Gui gui;
    if (!gui.initialize()) {
        consoleInit(nullptr);
        std::printf("NXSync %s\n\n", AppVersion);
        std::printf("Unable to initialize the GUI:\n%s\n\n", gui.error().c_str());
        std::printf("Press + to exit.\n");
        consoleUpdate(nullptr);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
                break;
            }
        }
        consoleExit(nullptr);
        return 1;
    }

    nxsync::AppConfig config = nxsync::loadOrCreateConfig(ConfigPath);
    const nxsync::DeviceIdentity identity = nxsync::detectDeviceIdentity(
        config.deviceIdOverride,
        FallbackIdPath);
    std::string remoteRoot = nxsync::makeDeviceRemoteRoot(
        config.remoteRoot,
        identity.folderName);

    gui.renderLoading(AppVersion, "Loading catalog", "Reading profiles and saves...");

    nxsync::SaveCatalog catalog = nxsync::loadSaveCatalog();
    std::string lastMessage;
    std::string lastArchivePath;
    nxsync::GuiTone lastMessageTone = nxsync::GuiTone::Accent;
    if (config.credentialMigrated) {
        lastMessage = "Nextcloud application password encrypted for this console";
        lastMessageTone = nxsync::GuiTone::Good;
    } else if (!config.credentialError.empty()) {
        lastMessage = config.credentialError;
        lastMessageTone = nxsync::GuiTone::Danger;
    }
    int overlayCatalogError = 0;
    if (catalog.ok() && !exportOverlayCatalog(catalog, overlayCatalogError)) {
        lastMessage = "Overlay catalog was not updated (errno "
            + std::to_string(overlayCatalogError) + ")";
        lastMessageTone = nxsync::GuiTone::Warning;
    }
    nxsync::PreflightRequest pendingPreflight;
    std::string pendingPreflightError;
    if (nxsync::loadPreflightRequest(
            PreflightRequestPath,
            pendingPreflight,
            pendingPreflightError)) {
        lastMessage = "Preflight " + pendingPreflight.titleId
            + " pending: return to HOME to start the worker";
        lastMessageTone = nxsync::GuiTone::Warning;
    } else {
        nxsync::PreflightStatus preflightStatus;
        std::string preflightStatusError;
        if (nxsync::loadPreflightStatus(
                PreflightStatusPath,
                preflightStatus,
                preflightStatusError)) {
            lastMessage = "Preflight " + preflightStatus.titleId + ": "
                + preflightStatus.message;
            if (!preflightStatus.selectedDeviceId.empty()) {
                lastMessage += " - " + preflightStatus.selectedDeviceId
                    + " / " + preflightStatus.selectedProfileName;
            }
            lastMessageTone = preflightStatus.state == "error"
                || preflightStatus.outcome == "conflict"
                || preflightStatus.outcome == "invalid-cloud-index"
                ? nxsync::GuiTone::Danger
                : (preflightStatus.outcome == "cloud-update-available"
                    || preflightStatus.state == "working"
                    ? nxsync::GuiTone::Warning
                    : nxsync::GuiTone::Good);
        }
    }
    if (!cloudWorkerLocked()
        && !config.autoBackupOnStart
        && config.nextcloudConfigured()
        && !nxsync::SyncEngine(CloudQueueRoot).pendingOperations().empty()) {
        const BatchBackupSummary retrySummary = retryPendingCloudOperations(
            gui,
            config,
            identity,
            catalog);
        lastMessage = "Cloud queue: " + std::to_string(retrySummary.uploaded)
            + " completed, " + std::to_string(retrySummary.uploadFailed)
            + " still pending";
        if (!retrySummary.firstError.empty()) {
            lastMessage += " - " + retrySummary.firstError;
        }
        lastMessageTone = retrySummary.uploadFailed == 0
            ? nxsync::GuiTone::Good
            : nxsync::GuiTone::Warning;
    }
    if (!cloudWorkerLocked()
        && config.autoBackupOnStart && catalog.ok() && catalog.totalSaves > 0) {
        const BatchBackupSummary startupSummary = createAllLocalBackups(
            gui,
            config,
            identity,
            catalog,
            false);
        lastMessage = describeBatchSummary(
            "Automatic backup",
            startupSummary,
            config.nextcloudConfigured());
        lastArchivePath = startupSummary.lastArchivePath;
        lastMessageTone = startupSummary.failed == 0
                && startupSummary.uploadFailed == 0
                && startupSummary.indexFailed == 0
                && startupSummary.retentionFailed == 0
            ? nxsync::GuiTone::Good
            : nxsync::GuiTone::Warning;
        if (showBatchBackupSummary(
                gui,
                identity,
                config,
                startupSummary,
                "Automatic backup",
                "Incremental check at startup")) {
            gui.shutdown();
            return 0;
        }
    }
    BackupStateCache backupStates = loadBackupStateCache(identity, catalog);
    std::size_t userIndex = 0;
    std::size_t pageIndex = 0;
    std::size_t selectedSaveIndex = 0;
    bool batchConfirmationPending = false;
    bool needsRedraw = true;

    while (appletMainLoop()) {
        if (needsRedraw) {
            renderCatalog(
                gui,
                identity,
                config,
                remoteRoot,
                catalog,
                backupStates,
                userIndex,
                pageIndex,
                selectedSaveIndex,
                lastMessage,
                lastArchivePath,
                lastMessageTone,
                batchConfirmationPending);
            needsRedraw = false;
        }

        padUpdate(&pad);
        const u64 buttonsDown = padGetButtonsDown(&pad);
        if ((buttonsDown & HidNpadButton_Plus) != 0) {
            break;
        }

        if ((buttonsDown & HidNpadButton_Minus) != 0) {
            batchConfirmationPending = false;
            const ConfigWizardResult wizard = runSettingsMenu(
                gui,
                identity.folderName,
                config);
            if (wizard.exitRequested) {
                break;
            }
            lastMessage = wizard.message;
            lastMessageTone = wizard.saved
                ? nxsync::GuiTone::Good
                : (wizard.message.find("Unable") != std::string::npos
                    || wizard.message.find("Invalid") != std::string::npos
                    ? nxsync::GuiTone::Danger
                    : nxsync::GuiTone::Neutral);
            lastArchivePath.clear();
            if (wizard.saved) {
                remoteRoot = nxsync::makeDeviceRemoteRoot(
                    config.remoteRoot,
                    identity.folderName);
                if (wizard.testConnection) {
                    gui.renderLoading(
                        AppVersion,
                        "Configuration saved",
                        "Testing HTTPS and Nextcloud authentication...");
                    nxsync::NextcloudClient client(config);
                    const nxsync::NextcloudResult test = client.ready()
                        ? client.testConnection()
                        : client.initializationResult();
                    lastMessage += test.success
                        ? "; " + test.message
                        : "; test failed: " + test.message;
                    lastMessageTone = test.success
                        ? nxsync::GuiTone::Good
                        : nxsync::GuiTone::Danger;
                    if (!test.success && test.httpStatus != 0) {
                        lastMessage += " (HTTP " + std::to_string(test.httpStatus) + ")";
                    }
                }
            }
            needsRedraw = true;
            continue;
        }

        if ((buttonsDown & HidNpadButton_X) != 0) {
            gui.renderLoading(AppVersion, "Refreshing catalog", "Reading saves...");
            catalog = nxsync::loadSaveCatalog();
            overlayCatalogError = 0;
            if (catalog.ok() && !exportOverlayCatalog(catalog, overlayCatalogError)) {
                lastMessage = "Overlay catalog was not updated (errno "
                    + std::to_string(overlayCatalogError) + ")";
                lastMessageTone = nxsync::GuiTone::Warning;
            }
            backupStates = loadBackupStateCache(identity, catalog);
            userIndex = 0;
            pageIndex = 0;
            selectedSaveIndex = 0;
            if (overlayCatalogError == 0) {
                lastMessage = "Homebrew and overlay catalogs refreshed";
                lastMessageTone = nxsync::GuiTone::Good;
            }
            lastArchivePath.clear();
            batchConfirmationPending = false;
            needsRedraw = true;
            continue;
        }

        if ((buttonsDown & HidNpadButton_ZL) != 0) {
            batchConfirmationPending = false;
            if (catalog.users.empty()
                || userIndex >= catalog.users.size()
                || catalog.users[userIndex].saves.empty()
                || selectedSaveIndex >= catalog.users[userIndex].saves.size()) {
                lastMessage = "Select a game before requesting preflight";
                lastMessageTone = nxsync::GuiTone::Warning;
                needsRedraw = true;
                continue;
            }
            nxsync::SysmoduleConfig sysmoduleConfig;
            std::string sysmoduleConfigError;
            if (!nxsync::loadSysmoduleConfig(
                    SysmoduleConfigPath,
                    sysmoduleConfig,
                    sysmoduleConfigError)
                || !sysmoduleConfig.enabled
                || !sysmoduleConfig.preflightEnabled) {
                lastMessage = "Enable Game launch preflight in Settings";
                lastMessageTone = nxsync::GuiTone::Warning;
                needsRedraw = true;
                continue;
            }
            if (!config.nextcloudConfigured()) {
                lastMessage = "Configure Nextcloud before running preflight";
                lastMessageTone = nxsync::GuiTone::Warning;
                needsRedraw = true;
                continue;
            }
            const nxsync::UserSaves& selectedUser = catalog.users[userIndex];
            const nxsync::SaveEntry& selectedSave =
                selectedUser.saves[selectedSaveIndex];
            nxsync::PreflightRequest request;
            request.sequence = armTicksToNs(armGetSystemTick());
            if (request.sequence == 0) request.sequence = 1;
            request.titleId = nxsync::formatTitleId(selectedSave.applicationId);
            request.profileUid = formatProfileUid(selectedUser.uid);
            int systemError = 0;
            if (!nxsync::writePreflightRequestAtomic(
                    PreflightRequestPath,
                    request,
                    systemError)) {
                lastMessage = "Unable to queue preflight (errno "
                    + std::to_string(systemError) + ")";
                lastMessageTone = nxsync::GuiTone::Danger;
            } else {
                lastMessage = "Preflight " + request.titleId
                    + " queued: press + and stay on HOME; reopen NXSync for the result";
                lastMessageTone = nxsync::GuiTone::Good;
            }
            lastArchivePath.clear();
            needsRedraw = true;
            continue;
        }

        if ((buttonsDown & HidNpadButton_ZR) != 0) {
            batchConfirmationPending = false;
            if (cloudWorkerLocked()) {
                lastMessage = "Wait for the cloud worker to finish before restoring";
                lastMessageTone = nxsync::GuiTone::Warning;
                needsRedraw = true;
                continue;
            }
            if (catalog.users.empty()
                || userIndex >= catalog.users.size()
                || catalog.users[userIndex].saves.empty()
                || selectedSaveIndex >= catalog.users[userIndex].saves.size()) {
                lastMessage = "Select a game before opening Restore";
                lastMessageTone = nxsync::GuiTone::Warning;
                lastArchivePath.clear();
                needsRedraw = true;
                continue;
            }
            const nxsync::SaveEntry& selectedSave =
                catalog.users[userIndex].saves[selectedSaveIndex];
            const RestoreUiResult restore = runRestoreBrowser(
                gui,
                config,
                identity,
                catalog,
                catalog.users[userIndex],
                selectedSave);
            if (restore.exitRequested) {
                break;
            }
            lastMessage = restore.message;
            lastMessageTone = restore.success
                ? nxsync::GuiTone::Good
                : (restore.catalogChanged
                    ? nxsync::GuiTone::Danger
                    : nxsync::GuiTone::Neutral);
            lastArchivePath = restore.safetyBackupPath;
            if (restore.catalogChanged) {
                gui.renderLoading(
                    AppVersion,
                    "Refreshing after restore",
                    "Reading profiles and saves again...");
                catalog = nxsync::loadSaveCatalog();
                overlayCatalogError = 0;
                exportOverlayCatalog(catalog, overlayCatalogError);
                backupStates = loadBackupStateCache(identity, catalog);
                userIndex = std::min(
                    userIndex,
                    catalog.users.empty() ? 0 : catalog.users.size() - 1);
                pageIndex = 0;
                selectedSaveIndex = 0;
            }
            needsRedraw = true;
            continue;
        }

        if (catalog.users.empty()) {
            continue;
        }

        if (batchConfirmationPending) {
            if ((buttonsDown & HidNpadButton_B) != 0) {
                batchConfirmationPending = false;
                lastMessage = "Global backup cancelled";
                lastMessageTone = nxsync::GuiTone::Neutral;
                lastArchivePath.clear();
                needsRedraw = true;
            } else if ((buttonsDown & HidNpadButton_Y) != 0) {
                batchConfirmationPending = false;
                if (cloudWorkerLocked()) {
                    lastMessage = "A cloud upload is already running in the background";
                    lastMessageTone = nxsync::GuiTone::Warning;
                    needsRedraw = true;
                    continue;
                }
                const BatchBackupSummary summary = createAllLocalBackups(
                    gui,
                    config,
                    identity,
                    catalog);
                backupStates = loadBackupStateCache(identity, catalog);
                lastMessage = describeBatchSummary(
                    "Global backup",
                    summary,
                    config.nextcloudConfigured());
                lastMessageTone = summary.failed == 0
                        && summary.uploadFailed == 0
                        && summary.indexFailed == 0
                        && summary.retentionFailed == 0
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Warning;
                lastArchivePath = summary.lastArchivePath;
                if (showBatchBackupSummary(
                        gui,
                        identity,
                        config,
                        summary,
                        "Global backup",
                        "Full local and Nextcloud reconciliation")) {
                    break;
                }
                needsRedraw = true;
            }
            continue;
        }

        if ((buttonsDown & HidNpadButton_Y) != 0 && catalog.totalSaves > 0) {
            batchConfirmationPending = true;
            lastMessage = "Press Y again to confirm the global backup";
            lastMessageTone = nxsync::GuiTone::Warning;
            lastArchivePath.clear();
            needsRedraw = true;
            continue;
        }

        if ((buttonsDown & HidNpadButton_B) != 0) {
            if (!config.nextcloudConfigured()) {
                lastMessage = "Nextcloud is not configured: press -";
                lastMessageTone = nxsync::GuiTone::Warning;
            } else {
                gui.renderLoading(
                    AppVersion,
                    "Testing Nextcloud connection",
                    "Checking HTTPS and WebDAV authentication...");
                nxsync::NextcloudClient client(config);
                const nxsync::NextcloudResult test = client.ready()
                    ? client.testConnection()
                    : client.initializationResult();
                lastMessage = test.message;
                lastMessageTone = test.success
                    ? nxsync::GuiTone::Good
                    : nxsync::GuiTone::Danger;
                if (!test.success && test.httpStatus != 0) {
                    lastMessage += " (HTTP " + std::to_string(test.httpStatus) + ")";
                }
            }
            lastArchivePath.clear();
            needsRedraw = true;
            continue;
        }

        if ((buttonsDown & HidNpadButton_L) != 0) {
            userIndex = userIndex == 0 ? catalog.users.size() - 1 : userIndex - 1;
            pageIndex = 0;
            selectedSaveIndex = 0;
            needsRedraw = true;
        } else if ((buttonsDown & HidNpadButton_R) != 0) {
            userIndex = (userIndex + 1) % catalog.users.size();
            pageIndex = 0;
            selectedSaveIndex = 0;
            needsRedraw = true;
        } else if ((buttonsDown & HidNpadButton_Up) != 0
            && !catalog.users[userIndex].saves.empty()) {
            const std::size_t saveCount = catalog.users[userIndex].saves.size();
            selectedSaveIndex = selectedSaveIndex == 0
                ? saveCount - 1
                : selectedSaveIndex - 1;
            pageIndex = selectedSaveIndex / SavesPerPage;
            needsRedraw = true;
        } else if ((buttonsDown & HidNpadButton_Down) != 0
            && !catalog.users[userIndex].saves.empty()) {
            const std::size_t saveCount = catalog.users[userIndex].saves.size();
            selectedSaveIndex = (selectedSaveIndex + 1) % saveCount;
            pageIndex = selectedSaveIndex / SavesPerPage;
            needsRedraw = true;
        } else if ((buttonsDown & HidNpadButton_Left) != 0) {
            const std::size_t totalPages = pageCount(catalog.users[userIndex]);
            pageIndex = pageIndex == 0 ? totalPages - 1 : pageIndex - 1;
            selectedSaveIndex = std::min(
                pageIndex * SavesPerPage,
                catalog.users[userIndex].saves.empty()
                    ? 0
                    : catalog.users[userIndex].saves.size() - 1);
            needsRedraw = true;
        } else if ((buttonsDown & HidNpadButton_Right) != 0) {
            const std::size_t totalPages = pageCount(catalog.users[userIndex]);
            pageIndex = (pageIndex + 1) % totalPages;
            selectedSaveIndex = std::min(
                pageIndex * SavesPerPage,
                catalog.users[userIndex].saves.empty()
                    ? 0
                    : catalog.users[userIndex].saves.size() - 1);
            needsRedraw = true;
        } else if ((buttonsDown & HidNpadButton_A) != 0
            && !catalog.users[userIndex].saves.empty()) {
            const nxsync::UserSaves& user = catalog.users[userIndex];
            const nxsync::SaveEntry& save = user.saves[selectedSaveIndex];
            if (cloudWorkerLocked()
                || nxsync::launchRestoreBlocksTitle(nxsync::formatTitleId(save.applicationId))) {
                lastMessage = "Backup paused: wait for the worker or recover the interrupted restore";
                lastMessageTone = nxsync::GuiTone::Warning;
                needsRedraw = true;
                continue;
            }
            BackupUiContext backupUi{&gui, &identity, &user, &save, 0, 0, 0};
            const nxsync::BackupResult backup = nxsync::createLocalBackup(
                identity,
                user,
                save,
                renderBackupProgress,
                &backupUi);
            lastMessage = backup.message;
            lastMessageTone = backup.success
                ? nxsync::GuiTone::Good
                : (backup.emptySave
                    ? nxsync::GuiTone::Neutral
                    : nxsync::GuiTone::Danger);
            lastArchivePath.clear();
            if (backup.success) {
                lastMessage += " - " + std::to_string(backup.fileCount)
                    + " file, SHA-256 " + backup.sha256.substr(0, 12);
                lastArchivePath = backup.archivePath;
                nxsync::LocalBackupState state;
                state = nxsync::findCurrentLocalBackup(identity, user, save);
                if (!state.current) {
                    state.current = true;
                    state.archivePath = backup.archivePath;
                    state.sha256 = backup.sha256;
                    state.revisionId = backup.revisionId;
                    state.parentRevisionId = backup.parentRevisionId;
                    state.parentRevisionIds = backup.parentRevisionIds;
                    state.payloadSha256 = backup.payloadSha256;
                    state.fileCount = backup.fileCount;
                    state.uncompressedBytes = backup.uncompressedBytes;
                }
                RetentionRunResult retention;
                if (config.nextcloudConfigured()) {
                    nxsync::NextcloudClient client(config);
                    nxsync::NextcloudResult upload;
                    if (client.ready()) {
                        upload = uploadBackup(
                            client,
                            config,
                            identity,
                            user,
                            save,
                            state,
                            backupUi);
                    } else {
                        upload = client.initializationResult();
                        const nxsync::SyncUploadPlan plan = makeSyncUploadPlan(
                            config,
                            identity,
                            user,
                            save,
                            state);
                        int queueError = 0;
                        nxsync::SyncEngine(CloudQueueRoot).defer(
                            plan,
                            upload.message,
                            queueError);
                    }
                    lastMessage += upload.success
                        ? "; cloud verified"
                        : "; cloud pending: " + upload.message;
                    if (!upload.success) {
                        lastMessageTone = nxsync::GuiTone::Warning;
                    }
                    const nxsync::LocalBackupState retentionState =
                        nxsync::findCurrentLocalBackup(identity, user, save);
                    retention = applyRetention(
                        config,
                        retentionState,
                        backupRemotePath(
                            config,
                            identity,
                            user,
                            save,
                            state.archivePath),
                        &client,
                        upload.success,
                        backupUi);
                } else {
                    const nxsync::LocalBackupState retentionState =
                        nxsync::findCurrentLocalBackup(identity, user, save);
                    retention = applyRetention(
                        config,
                        retentionState,
                        std::string(),
                        nullptr,
                        false,
                        backupUi);
                }
                if (retention.localRemoved > 0 || retention.remoteRemoved > 0) {
                    lastMessage += "; deleted "
                        + std::to_string(retention.localRemoved) + " local and "
                        + std::to_string(retention.remoteRemoved) + " cloud";
                }
                if (retention.failed > 0) {
                    lastMessage += "; retention: " + retention.firstError;
                    lastMessageTone = nxsync::GuiTone::Warning;
                }
            } else {
                lastMessage = describeBackupError(backup);
            }
            refreshBackupState(
                backupStates,
                identity,
                catalog,
                userIndex,
                selectedSaveIndex);
            needsRedraw = true;
        }
    }

    gui.shutdown();
    return 0;
}
