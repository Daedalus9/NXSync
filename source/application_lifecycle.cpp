#include "nxsync/application_lifecycle.hpp"

namespace nxsync {

bool recordApplicationStart(
    ApplicationLifecycleState& state,
    const std::uint64_t processId,
    const std::string& programId,
    const bool detectedAtStartup) {
    if (processId == 0 || programId.empty()) return false;
    if (state.activeProcessId == processId
        && state.activeProgramId == programId) return false;
    state.activeProcessId = processId;
    state.activeProgramId = programId;
    state.lastProgramId = programId;
    state.lastEvent = detectedAtStartup ? "detected" : "start";
    ++state.eventSequence;
    return true;
}

bool recordApplicationTermination(
    ApplicationLifecycleState& state,
    const std::uint64_t processId,
    const bool crashed) {
    if (processId == 0 || processId != state.activeProcessId) return false;
    state.lastProgramId = state.activeProgramId;
    state.lastEvent = crashed ? "crash" : "exit";
    state.activeProgramId.clear();
    state.activeProcessId = 0;
    ++state.eventSequence;
    return true;
}

bool reconcileMissingApplication(
    ApplicationLifecycleState& state,
    const std::uint64_t observedProcessId,
    const bool trackedProcessStillExists) {
    if (state.activeProcessId == 0
        || observedProcessId == state.activeProcessId
        || trackedProcessStillExists) {
        return false;
    }
    return recordApplicationTermination(
        state, state.activeProcessId, false);
}

} // namespace nxsync
