#include "nxsync/launch_protocol.hpp"

#include <cassert>
#include <string>
#include <filesystem>
#include <chrono>
#include <fstream>

int main() {
    nxsync::LaunchRequest request;
    request.sequence = 42;
    request.processId = 99;
    request.titleId = "010054001899e000";
    nxsync::LaunchRequest parsedRequest;
    std::string error;
    assert(nxsync::parseLaunchRequest(
        nxsync::serializeLaunchRequest(request), parsedRequest, error));
    assert(parsedRequest.sequence == 42);
    assert(parsedRequest.processId == 99);
    assert(parsedRequest.titleId == "010054001899E000");

    nxsync::LaunchDecision decision;
    decision.sequence = request.sequence;
    decision.action = "allow";
    decision.message = "cloud=ok\nlocale";
    nxsync::LaunchDecision parsedDecision;
    assert(nxsync::parseLaunchDecision(
        nxsync::serializeLaunchDecision(decision), parsedDecision, error));
    assert(parsedDecision.message == decision.message);

    nxsync::LaunchAction action;
    action.sequence = request.sequence;
    action.action = "restore-cloud";
    action.selectedRevisionId = std::string(64, 'a');
    nxsync::LaunchAction parsedAction;
    assert(nxsync::parseLaunchAction(
        nxsync::serializeLaunchAction(action), parsedAction, error));
    assert(parsedAction.action == "restore-cloud");
    assert(parsedAction.selectedRevisionId == action.selectedRevisionId);

    assert(!nxsync::parseLaunchRequest(
        "version=1\nsequence=0\nprocess_id=1\ntitle_id=BAD\n",
        parsedRequest,
        error));
    assert(!nxsync::parseLaunchRequest(
        "nxsync-launch-gate\n"
        "text\":\"Mario Kart 8 Deluxe: cloud check in p",
        parsedRequest,
        error));
    assert(error == "Invalid launch request");

    request.titleId = "010054001899E000";
    nxsync::LaunchRestoreLease lease{request, std::string(32, 'A')}, parsedLease;
    const auto wire = nxsync::serializeLaunchRestoreLease(lease);
    assert(nxsync::parseLaunchRestoreLease(wire, parsedLease, error));
    assert(parsedLease.profileUid == lease.profileUid);
    assert(!nxsync::parseLaunchRestoreLease(wire + "action=allow\n", parsedLease, error));
    assert(!nxsync::parseLaunchRestoreLease(wire.substr(0, wire.size()-1), parsedLease, error));
    decision.action = "abort";
    assert(nxsync::parseLaunchDecision(nxsync::serializeLaunchDecision(decision), parsedDecision, error));
    auto invalid = nxsync::serializeLaunchDecision(decision);
    assert(!nxsync::parseLaunchDecision(invalid + "action=allow\n", parsedDecision, error));
    assert(!nxsync::parseLaunchDecision("version=2\nsequence=-1\naction=allow\nmessage=\n", parsedDecision, error));
    assert(!nxsync::parseLaunchDecision("version=4294967298\nsequence=1\naction=allow\nmessage=\n", parsedDecision, error));
    const auto root = std::filesystem::temp_directory_path() / ("nxsync-lease-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    const auto guard = (root / "guard").string();
    int systemError = 0;
    const auto phasePath = (root / "phase").string();
    assert(nxsync::writeLaunchPhaseAtomic(phasePath, 42, "choice", systemError));
    {
        std::ifstream file(phasePath);
        const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        assert(content == "version=2\nsequence=42\nphase=choice\n");
    }
    assert(nxsync::writeLaunchPhaseAtomic(phasePath, 42, "download", systemError));
    assert(!nxsync::writeLaunchPhaseAtomic(phasePath, 0, "choice", systemError));
    assert(!nxsync::writeLaunchPhaseAtomic(phasePath, 42, "choice\nphase=download", systemError));
    assert(!nxsync::launchRestoreBlocksTitle(lease.request.titleId, guard));
    assert(nxsync::writeLaunchRestoreLease(guard, lease, systemError));
    assert(nxsync::launchRestoreBlocksTitle(lease.request.titleId, guard));
    assert(nxsync::launchRestoreBlocksTitle("010054001899e000", guard));
    assert(!nxsync::launchRestoreBlocksTitle("0100000000000002", guard));
    assert(!nxsync::clearRecoveredLaunchGuard(lease.request.titleId, std::string(32, 'B'), systemError, guard));
    assert(std::filesystem::exists(guard));
    assert(!nxsync::clearRecoveredLaunchGuard("0100000000000002", lease.profileUid, systemError, guard));
    std::filesystem::rename(guard, guard + ".bak");
    assert(nxsync::launchRestoreBlocksTitle(lease.request.titleId, guard));
    assert(nxsync::clearRecoveredLaunchGuard(lease.request.titleId, lease.profileUid, systemError, guard));
    assert(!std::filesystem::exists(guard + ".bak"));
    assert(nxsync::clearRecoveredLaunchGuard(lease.request.titleId, lease.profileUid, systemError, guard));
    assert(!nxsync::launchRestoreBlocksTitle(lease.request.titleId, guard));
    { std::ofstream broken(guard); broken << "truncated"; }
    assert(nxsync::launchRestoreBlocksTitle("0100000000000002", guard));
    assert(!nxsync::clearRecoveredLaunchGuard(lease.request.titleId, lease.profileUid, systemError, guard));
    std::filesystem::remove_all(root);
    assert(!nxsync::launchRestoreBlocksTitle(lease.request.titleId, guard));
    assert(nxsync::clearRecoveredLaunchGuard(lease.request.titleId, lease.profileUid, systemError, guard));
    assert(systemError == 0);
    return 0;
}
