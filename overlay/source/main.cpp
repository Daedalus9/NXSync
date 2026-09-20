#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include "nxsync/overlay_catalog.hpp"
#include "nxsync/launch_protocol.hpp"
#include "nxsync/preflight_protocol.hpp"
#include "nxsync/sysmodule_config.hpp"
#include "nxsync/sysmodule_status.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* OverlayVersion = "0.3.22-rc1";
constexpr const char* CatalogPath = "sdmc:/config/NXSync/overlay.catalog";
constexpr const char* SysmoduleConfigPath = "sdmc:/config/NXSync/sysmodule.ini";
constexpr const char* SysmoduleStatusPath = "sdmc:/config/NXSync/sysmodule.status";
constexpr const char* PreflightRequestPath = "sdmc:/config/NXSync/preflight.request";
constexpr const char* PreflightStatusPath = "sdmc:/config/NXSync/preflight.status";
constexpr const char* LaunchRequestPath = "sdmc:/config/NXSync/launch.request";
constexpr const char* LaunchDecisionPath = "sdmc:/config/NXSync/launch.decision";
constexpr const char* LaunchActionPath = "sdmc:/config/NXSync/launch.action";

struct ProfileChoice {
    std::string uid;
    std::string name;
    std::size_t games{0};
};

std::string readableCloudTimestamp(const std::string& value) {
    if (value.size() >= 16 && value[4] == '-' && value[7] == '-'
        && value[10] == 'T' && value[13] == ':') {
        return value.substr(8, 2) + "/" + value.substr(5, 2) + "/"
            + value.substr(0, 4) + " " + value.substr(11, 5) + " UTC";
    }
    return value.empty() ? "Unavailable" : value;
}

std::string preflightOutcomeMessage(const nxsync::PreflightStatus& preflight) {
    if (preflight.outcome == "conflict") {
        return "Revision conflict";
    }
    if (!preflight.message.empty()) return preflight.message;
    if (preflight.outcome == "cloud-update-available") {
        return "Newer cloud revision available";
    }
    if (preflight.outcome == "synchronized") {
        return "Local and cloud saves are synchronized";
    }
    if (preflight.outcome == "local-newer") {
        return "The local save is newer";
    }
    return "Result unavailable";
}

struct Snapshot {
    nxsync::OverlayCatalog catalog;
    nxsync::SysmoduleStatus sysmodule;
    nxsync::SysmoduleConfig configuration;
    nxsync::PreflightRequest request;
    nxsync::PreflightStatus preflight;
    nxsync::LaunchRequest launchRequest;
    nxsync::LaunchDecision launchDecision;
    bool catalogAvailable{false};
    bool sysmoduleAvailable{false};
    bool configurationAvailable{false};
    bool requestPending{false};
    bool preflightAvailable{false};
    bool launchPending{false};
    bool launchResolved{false};
    std::string catalogError;
    std::string sysmoduleError;
    std::string configurationError;
};

Snapshot loadSnapshot() {
    Snapshot snapshot;
    snapshot.catalogAvailable = nxsync::loadOverlayCatalog(
        CatalogPath, snapshot.catalog, snapshot.catalogError);
    snapshot.sysmoduleAvailable = nxsync::loadSysmoduleStatus(
        SysmoduleStatusPath, snapshot.sysmodule, snapshot.sysmoduleError);
    snapshot.configurationAvailable = nxsync::loadSysmoduleConfig(
        SysmoduleConfigPath,
        snapshot.configuration,
        snapshot.configurationError);
    std::string requestError;
    snapshot.requestPending = nxsync::loadPreflightRequest(
        PreflightRequestPath, snapshot.request, requestError);
    std::string statusError;
    snapshot.preflightAvailable = nxsync::loadPreflightStatus(
        PreflightStatusPath, snapshot.preflight, statusError);
    std::string launchError;
    const bool hasLaunchRequest = nxsync::loadLaunchRequest(
        LaunchRequestPath, snapshot.launchRequest, launchError);
    std::string decisionError;
    snapshot.launchResolved = hasLaunchRequest
        && nxsync::loadLaunchDecision(
            LaunchDecisionPath, snapshot.launchDecision, decisionError)
        && snapshot.launchDecision.sequence == snapshot.launchRequest.sequence;
    snapshot.launchPending = hasLaunchRequest && !snapshot.launchResolved;
    return snapshot;
}

std::vector<ProfileChoice> collectProfiles(const nxsync::OverlayCatalog& catalog) {
    std::vector<ProfileChoice> profiles;
    for (const nxsync::OverlayCatalogEntry& entry : catalog.entries) {
        const auto found = std::find_if(
            profiles.begin(), profiles.end(), [&](const ProfileChoice& profile) {
                return profile.uid == entry.profileUid;
            });
        if (found == profiles.end()) {
            profiles.push_back(ProfileChoice{
                entry.profileUid,
                entry.profileName,
                1});
        } else {
            ++found->games;
        }
    }
    return profiles;
}

class MessageGui : public tsl::Gui {
public:
    MessageGui(std::string title, std::string message, std::string value = {})
        : title_(std::move(title)), message_(std::move(message)), value_(std::move(value)) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(
            "NXSync", std::string("Overlay ") + OverlayVersion);
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader(title_));
        list->addItem(new tsl::elm::ListItem(message_, value_));
        list->addItem(new tsl::elm::CategoryHeader("B: back"));
        frame->setContent(list);
        return frame;
    }

private:
    std::string title_;
    std::string message_;
    std::string value_;
};

class LaunchRestoreWaitingGui : public tsl::Gui {
public:
    explicit LaunchRestoreWaitingGui(const std::uint64_t sequence)
        : sequence_(sequence) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(
            "NXSync", std::string("Overlay ") + OverlayVersion);
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("Cloud restore in progress"));
        list->addItem(new tsl::elm::CategoryHeader(
            "Downloading and verifying the backup", false));
        list->addItem(new tsl::elm::CategoryHeader(
            "Writing save data", false));
        list->addItem(new tsl::elm::CategoryHeader(
            "Please wait. Do not power off.", false));
        list->addItem(new tsl::elm::CategoryHeader(
            "The game will start automatically", false));
        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (++frames_ % 30 != 0) return;
        nxsync::LaunchDecision decision;
        std::string error;
        if (nxsync::loadLaunchDecision(
                LaunchDecisionPath, decision, error)
            && decision.sequence == sequence_) {
            tsl::Overlay::get()->close(true);
        }
    }

private:
    std::uint64_t sequence_{0};
    unsigned frames_{0};
};

class LaunchChoiceGui : public tsl::Gui {
public:
    LaunchChoiceGui(
        nxsync::LaunchRequest request,
        nxsync::PreflightStatus preflight)
        : request_(std::move(request)), preflight_(std::move(preflight)) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(
            "NXSync", "Sync before launch");
        auto* list = new tsl::elm::List();
        const auto addStaticInfo = [list](
            const std::string& label,
            const std::string& value) {
            auto* item = new tsl::elm::CategoryHeader(label, false);
            item->setValue(value.empty() ? "Unknown" : value);
            list->addItem(item);
        };
        list->addItem(new tsl::elm::CategoryHeader(request_.titleId));
        list->addItem(new tsl::elm::CategoryHeader(
            "Preflight result - information only"));
        addStaticInfo("Status", preflightOutcomeMessage(preflight_));

        auto addCloudChoice = [&](
            const std::string& heading,
            const std::string& deviceId,
            const std::string& profileName,
            const std::string& createdUtc,
            const std::string& gameVersion,
            const std::string& revisionId) {
            list->addItem(new tsl::elm::CategoryHeader(
                heading + " - information"));
            addStaticInfo("Console", deviceId.empty() ? "Unknown" : deviceId);
            addStaticInfo(
                "Profile",
                profileName.empty() ? "Not recorded" : profileName);
            addStaticInfo("Backup date", readableCloudTimestamp(createdUtc));
            addStaticInfo(
                "Game version",
                gameVersion.empty() ? "Not recorded" : gameVersion);
            list->addItem(new tsl::elm::CategoryHeader("Available action"));
            auto* cloud = new tsl::elm::ListItem(
                "Restore this cloud backup", "Press A");
            const std::uint64_t sequence = request_.sequence;
            cloud->setClickListener([sequence, revisionId](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                nxsync::LaunchAction action;
                action.sequence = sequence;
                action.action = "restore-cloud";
                action.selectedRevisionId = revisionId;
                int systemError = 0;
                if (!nxsync::writeLaunchActionAtomic(
                        LaunchActionPath, action, systemError)) {
                    tsl::changeTo<MessageGui>(
                        "Error",
                        "Unable to request cloud restore",
                        "errno " + std::to_string(systemError));
                    return true;
                }
                tsl::changeTo<LaunchRestoreWaitingGui>(sequence);
                return true;
            });
            list->addItem(cloud);
        };

        if (preflight_.outcome == "conflict"
            && !preflight_.candidates.empty()) {
            list->addItem(new tsl::elm::CategoryHeader(
                "Choose a cloud backup to restore"));
            for (std::size_t index = 0;
                 index < preflight_.candidates.size();
                 ++index) {
                const nxsync::PreflightCandidate& candidate =
                    preflight_.candidates[index];
                addCloudChoice(
                    "Backup " + std::to_string(index + 1) + " of "
                        + std::to_string(preflight_.candidates.size()),
                    candidate.deviceId,
                    candidate.profileName,
                    candidate.createdUtc,
                    candidate.gameVersion,
                    candidate.revisionId);
            }
        } else if (!preflight_.selectedArchivePath.empty()) {
            addCloudChoice(
                "Cloud backup available",
                preflight_.selectedDeviceId,
                preflight_.selectedProfileName,
                preflight_.selectedCreatedUtc,
                preflight_.selectedGameVersion,
                preflight_.selectedRevisionId);
        }

        list->addItem(new tsl::elm::CategoryHeader("Local save - action"));
        auto* local = new tsl::elm::ListItem(
            "Continue with local save", "Press A");
        const std::uint64_t sequence = request_.sequence;
        const std::string resolvedCloudRevision =
            (preflight_.outcome == "conflict"
                || preflight_.outcome == "cloud-update-available")
                && preflight_.candidates.size() == 1
            ? preflight_.candidates.front().revisionId
            : std::string();
        local->setClickListener([sequence, resolvedCloudRevision](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            nxsync::LaunchAction action;
            action.sequence = sequence;
            action.action = "use-local";
            action.selectedRevisionId = resolvedCloudRevision;
            int systemError = 0;
            if (!nxsync::writeLaunchActionAtomic(
                    LaunchActionPath, action, systemError)) {
                tsl::changeTo<MessageGui>(
                    "Error",
                    "Unable to authorize the local save",
                    "errno " + std::to_string(systemError));
                return true;
            }
            tsl::Overlay::get()->close(true);
            return true;
        });
        list->addItem(local);
        list->addItem(new tsl::elm::CategoryHeader(
            "Timeout automatically uses the local save", false));
        frame->setContent(list);
        return frame;
    }

private:
    nxsync::LaunchRequest request_;
    nxsync::PreflightStatus preflight_;
};

class TitleGui : public tsl::Gui {
public:
    TitleGui(std::string profileUid, std::string profileName)
        : profileUid_(std::move(profileUid)), profileName_(std::move(profileName)) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(
            "NXSync", profileName_ + " - choose a game");
        auto* list = new tsl::elm::List();
        nxsync::OverlayCatalog catalog;
        std::string error;
        if (!nxsync::loadOverlayCatalog(CatalogPath, catalog, error)) {
            list->addItem(new tsl::elm::CategoryHeader("Catalog unavailable"));
            list->addItem(new tsl::elm::ListItem(error));
            frame->setContent(list);
            return frame;
        }

        for (const nxsync::OverlayCatalogEntry& entry : catalog.entries) {
            if (entry.profileUid != profileUid_) continue;
            auto* item = new tsl::elm::ListItem(entry.titleName, entry.titleId);
            const std::string titleId = entry.titleId;
            const std::string profileUid = entry.profileUid;
            item->setClickListener([titleId, profileUid](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;

                nxsync::SysmoduleStatus sysmodule;
                std::string statusError;
                if (!nxsync::loadSysmoduleStatus(
                        SysmoduleStatusPath, sysmodule, statusError)
                    || !sysmodule.enabled || !sysmodule.launchGateEnabled) {
                    tsl::changeTo<MessageGui>(
                        "Preflight unavailable",
                        "Enable the sysmodule and preflight in the homebrew app");
                    return true;
                }
                if (sysmodule.activeProcessId != 0) {
                    tsl::changeTo<MessageGui>(
                        "Close the game first",
                        "Run preflight from HOME with no game open",
                        sysmodule.activeProgramId);
                    return true;
                }

                nxsync::PreflightRequest existing;
                std::string requestError;
                if (nxsync::loadPreflightRequest(
                        PreflightRequestPath, existing, requestError)) {
                    tsl::changeTo<MessageGui>(
                        "Request already queued",
                        "Wait for completion before queuing another request",
                        existing.titleId);
                    return true;
                }

                nxsync::PreflightRequest request;
                request.sequence = armTicksToNs(armGetSystemTick());
                if (request.sequence == 0) request.sequence = 1;
                request.titleId = titleId;
                request.profileUid = profileUid;
                int systemError = 0;
                if (!nxsync::writePreflightRequestAtomic(
                        PreflightRequestPath, request, systemError)) {
                    tsl::changeTo<MessageGui>(
                        "Queue error",
                        "Unable to write the request",
                        "errno " + std::to_string(systemError));
                    return true;
                }
                tsl::changeTo<MessageGui>(
                    "Preflight queued",
                    "Close the overlay and wait for the worker result",
                    titleId);
                return true;
            });
            list->addItem(item);
        }
        frame->setContent(list);
        return frame;
    }

private:
    std::string profileUid_;
    std::string profileName_;
};

class ProfileGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(
            "NXSync", "Choose the local profile");
        auto* list = new tsl::elm::List();
        nxsync::OverlayCatalog catalog;
        std::string error;
        if (!nxsync::loadOverlayCatalog(CatalogPath, catalog, error)) {
            list->addItem(new tsl::elm::CategoryHeader("Catalog unavailable"));
            list->addItem(new tsl::elm::ListItem(error));
            frame->setContent(list);
            return frame;
        }
        const std::vector<ProfileChoice> profiles = collectProfiles(catalog);
        for (const ProfileChoice& profile : profiles) {
            auto* item = new tsl::elm::ListItem(
                profile.name,
                std::to_string(profile.games) + " games");
            const std::string uid = profile.uid;
            const std::string name = profile.name;
            item->setClickListener([uid, name](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                tsl::changeTo<TitleGui>(uid, name);
                return true;
            });
            list->addItem(item);
        }
        frame->setContent(list);
        return frame;
    }
};

class MainGui : public tsl::Gui {
public:
    explicit MainGui(const bool focusRefresh = false)
        : focusRefresh_(focusRefresh) {}

    tsl::elm::Element* createUI() override {
        const Snapshot snapshot = loadSnapshot();
        auto* frame = new tsl::elm::OverlayFrame(
            "NXSync", std::string("Overlay ") + OverlayVersion);
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Sysmodule"));
        if (snapshot.sysmoduleAvailable) {
            list->addItem(new tsl::elm::ListItem(
                snapshot.sysmodule.state,
                snapshot.sysmodule.enabled ? "enabled" : "disabled"));
            list->addItem(new tsl::elm::ListItem(
                "Build " + snapshot.sysmodule.buildVersion,
                std::to_string(snapshot.sysmodule.pendingOperations) + " cloud"));
            if (!snapshot.sysmodule.lastError.empty()) {
                list->addItem(new tsl::elm::ListItem(
                    "Last warning", snapshot.sysmodule.lastError));
            }
        } else {
            list->addItem(new tsl::elm::ListItem(
                "Status unavailable", snapshot.sysmoduleError));
        }

        list->addItem(new tsl::elm::CategoryHeader("NAND environment"));
        const std::string environment = !snapshot.sysmoduleAvailable
            ? "Unavailable"
            : (snapshot.sysmodule.storageEnvironment == "emummc"
                ? "emuMMC"
                : (snapshot.sysmodule.storageEnvironment == "sysmmc"
                    ? "sysMMC"
                    : "Unknown"));
        list->addItem(new tsl::elm::ListItem(
            "Current environment", environment));

        auto* scopeItem = new tsl::elm::ListItem(
            "Automation scope",
            snapshot.configurationAvailable
                ? (snapshot.configuration.emummcOnly
                    ? "emuMMC only"
                    : "emuMMC + sysMMC")
                : "Unavailable");
        scopeItem->setClickListener([snapshot](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            if (!snapshot.configurationAvailable) {
                tsl::changeTo<MessageGui>(
                    "Configuration unavailable",
                    snapshot.configurationError);
                return true;
            }
            nxsync::SysmoduleConfig updated = snapshot.configuration;
            updated.emummcOnly = !updated.emummcOnly;
            int systemError = 0;
            if (!nxsync::writeSysmoduleConfig(
                    SysmoduleConfigPath, updated, systemError)) {
                tsl::changeTo<MessageGui>(
                    "Error",
                    "Unable to update the automation scope",
                    "errno " + std::to_string(systemError));
                return true;
            }
            tsl::swapTo<MainGui>();
            return true;
        });
        list->addItem(scopeItem);
        list->addItem(new tsl::elm::ListItem(
            "Effective automation",
            snapshot.sysmoduleAvailable
                ? (snapshot.sysmodule.automationsAllowed
                    ? "Enabled"
                    : "Blocked by scope")
                : "Unavailable"));

        list->addItem(new tsl::elm::CategoryHeader("Preflight"));
        if (snapshot.requestPending) {
            list->addItem(new tsl::elm::ListItem(
                "Pending request", snapshot.request.titleId));
        } else if (snapshot.launchResolved) {
            list->addItem(new tsl::elm::ListItem(
                snapshot.launchDecision.message.empty()
                    ? "Launch authorized"
                    : snapshot.launchDecision.message,
                "completed"));
        } else if (snapshot.launchPending
            && (!snapshot.preflightAvailable
                || snapshot.preflight.sequence
                    != snapshot.launchRequest.sequence)) {
            list->addItem(new tsl::elm::ListItem(
                "Automatic check in progress",
                snapshot.launchRequest.titleId));
        } else if (snapshot.preflightAvailable) {
            list->addItem(new tsl::elm::ListItem(
                snapshot.preflight.message,
                snapshot.preflight.outcome.empty()
                    ? snapshot.preflight.state
                    : snapshot.preflight.outcome));
        } else {
            list->addItem(new tsl::elm::ListItem("No result available"));
        }

        auto* queueItem = new tsl::elm::ListItem("New preflight");
        queueItem->setClickListener([snapshot](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            if (!snapshot.catalogAvailable || snapshot.catalog.entries.empty()) {
                tsl::changeTo<MessageGui>(
                    "Catalog unavailable",
                    snapshot.catalogError.empty()
                        ? "Open NXSync 0.30.9-rc1 and press X to refresh it"
                        : snapshot.catalogError);
            } else if (!snapshot.sysmoduleAvailable
                || !snapshot.sysmodule.enabled
                || !snapshot.sysmodule.launchGateEnabled) {
                tsl::changeTo<MessageGui>(
                    "Preflight disabled",
                    "Enable it in the homebrew app settings");
            } else if (snapshot.requestPending) {
                tsl::changeTo<MessageGui>(
                    "Request already queued",
                    "Close the overlay and wait for the worker",
                    snapshot.request.titleId);
            } else {
                tsl::changeTo<ProfileGui>();
            }
            return true;
        });
        list->addItem(queueItem);

        auto* refreshItem = new tsl::elm::ListItem("Refresh status");
        refreshItem->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            tsl::swapTo<MainGui>(true);
            return true;
        });
        list->addItem(refreshItem);

        list->addItem(new tsl::elm::CategoryHeader("Catalog"));
        if (snapshot.catalogAvailable) {
            const std::vector<ProfileChoice> profiles = collectProfiles(snapshot.catalog);
            list->addItem(new tsl::elm::ListItem(
                std::to_string(snapshot.catalog.entries.size()) + " games",
                std::to_string(profiles.size()) + " profiles"));
        } else {
            list->addItem(new tsl::elm::ListItem(
                "Open the homebrew app to create it", snapshot.catalogError));
        }

        if (focusRefresh_) {
            list->jumpToItem("Refresh status");
        }

        frame->setContent(list);
        return frame;
    }

private:
    bool focusRefresh_{false};
};

class NXSyncOverlay : public tsl::Overlay {
public:
    void initServices() override {}

    void releasePendingLaunchToLocalSave() {
        nxsync::LaunchRequest request;
        std::string requestError;
        if (!nxsync::loadLaunchRequest(
                LaunchRequestPath, request, requestError)) {
            return;
        }
        nxsync::LaunchDecision decision;
        std::string decisionError;
        if (nxsync::loadLaunchDecision(
                LaunchDecisionPath, decision, decisionError)
            && decision.sequence == request.sequence) {
            return;
        }
        nxsync::LaunchAction existingAction;
        std::string actionError;
        if (nxsync::loadLaunchAction(
                LaunchActionPath, existingAction, actionError)
            && existingAction.sequence == request.sequence) {
            return;
        }
        nxsync::LaunchAction localAction;
        localAction.sequence = request.sequence;
        localAction.action = "use-local";
        int systemError = 0;
        nxsync::writeLaunchActionAtomic(
            LaunchActionPath, localAction, systemError);
    }

    void onHide() override {
        // HOME hides a Tesla overlay without terminating its process. Release
        // an unresolved launch here as well as during final shutdown, otherwise
        // dmnt keeps the application suspended and "Close software" can wait
        // until the gate timeout expires.
        releasePendingLaunchToLocalSave();
    }

    void exitServices() override {
        releasePendingLaunchToLocalSave();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        nxsync::LaunchRequest request;
        nxsync::LaunchDecision decision;
        nxsync::PreflightStatus preflight;
        std::string requestError;
        std::string decisionError;
        std::string preflightError;
        const bool hasRequest = nxsync::loadLaunchRequest(
            LaunchRequestPath, request, requestError);
        const bool hasResolvedDecision = hasRequest
            && nxsync::loadLaunchDecision(
                LaunchDecisionPath, decision, decisionError)
            && decision.sequence == request.sequence;
        if (hasRequest
            && !hasResolvedDecision
            && nxsync::loadPreflightStatus(
                PreflightStatusPath, preflight, preflightError)
            && request.sequence == preflight.sequence
            && preflight.state == "completed"
            && (preflight.outcome == "cloud-update-available"
                || preflight.outcome == "conflict")) {
            return initially<LaunchChoiceGui>(
                std::move(request), std::move(preflight));
        }
        return initially<MainGui>();
    }
};

} // namespace

int main(int argc, char** argv) {
    return tsl::loop<NXSyncOverlay>(argc, argv);
}
