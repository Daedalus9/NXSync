#pragma once

#include <cstdint>
#include <string>

namespace nxsync {

struct ApplicationLifecycleState {
    std::string activeProgramId;
    std::uint64_t activeProcessId{0};
    std::string lastProgramId;
    std::string lastEvent;
    std::uint64_t eventSequence{0};
};

bool recordApplicationStart(
    ApplicationLifecycleState& state,
    std::uint64_t processId,
    const std::string& programId,
    bool detectedAtStartup = false);

bool recordApplicationTermination(
    ApplicationLifecycleState& state,
    std::uint64_t processId,
    bool crashed);

bool reconcileMissingApplication(
    ApplicationLifecycleState& state,
    std::uint64_t observedProcessId,
    bool trackedProcessStillExists);

} // namespace nxsync
