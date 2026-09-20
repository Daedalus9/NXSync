#pragma once

#include <cstdint>
#include <string>

namespace nxsync {

constexpr unsigned LaunchProtocolVersion = 2;
inline constexpr const char* LaunchRestoreClaimPath = "sdmc:/config/NXSync/launch.restore-claim";
inline constexpr const char* LaunchRestoreGrantPath = "sdmc:/config/NXSync/launch.restore-grant";
inline constexpr const char* LaunchRestoreGuardPath = "sdmc:/config/NXSync/launch.restore-guard";

inline constexpr const char* LaunchPhasePath = "sdmc:/config/NXSync/launch.phase";
// Extends only the named phase of this session; repeating it cannot reset the timer.
bool writeLaunchPhaseAtomic(const std::string& path, std::uint64_t sequence,
                            const std::string& phase, int& systemError);

struct LaunchRequest {
    unsigned version{LaunchProtocolVersion};
    std::uint64_t sequence{0};
    std::uint64_t processId{0};
    std::string titleId;
};

struct LaunchDecision {
    unsigned version{LaunchProtocolVersion};
    std::uint64_t sequence{0};
    std::string action;
    std::string message;
};

struct LaunchAction {
    unsigned version{LaunchProtocolVersion};
    std::uint64_t sequence{0};
    std::string action;
    std::string selectedRevisionId;
};

// The gate persists the guard BEFORE granting permission to touch save data.
// It survives worker failures/reboots and is cleared only after a safe result.
struct LaunchRestoreLease {
    LaunchRequest request;
    std::string profileUid;
};
std::string serializeLaunchRestoreLease(const LaunchRestoreLease& lease);
bool parseLaunchRestoreLease(const std::string& text, LaunchRestoreLease& lease, std::string& error);
bool loadLaunchRestoreLease(const std::string& path, LaunchRestoreLease& lease, std::string& error);
bool writeLaunchRestoreLease(const std::string& path, const LaunchRestoreLease& lease, int& error);
// Pause backup/upload/retention for a title whose save may require recovery.
// An unreadable or malformed marker blocks all titles conservatively.
bool launchRestoreBlocksTitle(const std::string& titleId,
                              const std::string& path = LaunchRestoreGuardPath);
bool clearRecoveredLaunchGuard(const std::string& titleId, const std::string& profileUid, int& error,
                               const std::string& path = LaunchRestoreGuardPath);

std::string serializeLaunchRequest(const LaunchRequest& request);
bool parseLaunchRequest(
    const std::string& text,
    LaunchRequest& request,
    std::string& error);
bool loadLaunchRequest(
    const std::string& path,
    LaunchRequest& request,
    std::string& error);

std::string serializeLaunchDecision(const LaunchDecision& decision);
bool parseLaunchDecision(
    const std::string& text,
    LaunchDecision& decision,
    std::string& error);
bool loadLaunchDecision(
    const std::string& path,
    LaunchDecision& decision,
    std::string& error);
bool writeLaunchDecisionAtomic(
    const std::string& path,
    const LaunchDecision& decision,
    int& systemError);

std::string serializeLaunchAction(const LaunchAction& action);
bool parseLaunchAction(
    const std::string& text,
    LaunchAction& action,
    std::string& error);
bool loadLaunchAction(
    const std::string& path,
    LaunchAction& action,
    std::string& error);
bool writeLaunchActionAtomic(
    const std::string& path,
    const LaunchAction& action,
    int& systemError);
bool completeLaunchAction(
    const std::string& path,
    std::uint64_t expectedSequence,
    int& systemError);

bool requestLaunchOverlay(const std::string& path, int& systemError);

} // namespace nxsync
