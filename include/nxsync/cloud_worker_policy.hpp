#pragma once

#include <cstddef>
#include <cstdint>

namespace nxsync {

constexpr std::uint32_t CloudWorkerBackgroundNetworkWaitMs = 12'000;
constexpr std::uint32_t CloudWorkerInteractiveNetworkWaitMs = 2'000;
constexpr std::uint32_t CloudWorkerNetworkPollIntervalMs = 500;
constexpr std::size_t CloudTransferAttemptLimit = 3;

constexpr std::uint32_t cloudWorkerNetworkWaitBudgetMs(
    const bool interactiveRequest) {
    return interactiveRequest
        ? CloudWorkerInteractiveNetworkWaitMs
        : CloudWorkerBackgroundNetworkWaitMs;
}

constexpr std::uint32_t cloudTransferRetryDelaySeconds(
    const std::size_t failedAttemptIndex) {
    return failedAttemptIndex == 0 ? 1 : 2;
}

constexpr bool cloudHttpStatusIsTransient(const long httpStatus) {
    return httpStatus == 423
        || httpStatus == 429
        || httpStatus == 500
        || httpStatus == 502
        || httpStatus == 503
        || httpStatus == 504;
}

constexpr std::uint32_t cloudHttpRetryDelaySeconds(
    const long httpStatus,
    const std::size_t failedAttemptIndex) {
    if (httpStatus == 423 || httpStatus == 429) {
        return failedAttemptIndex == 0 ? 5 : 15;
    }
    return failedAttemptIndex == 0 ? 2 : 5;
}

// A clean worker exit after draining the queue must not delay the next upload.
// Backoff is reserved for crashes and for exits that leave work pending.
constexpr bool cloudWorkerNeedsRetry(
    const bool crashed,
    const std::size_t pendingOperations) {
    return crashed || pendingOperations > 0;
}

constexpr bool cloudWorkerMayUseApplicationSlot(
    const bool automaticLaunchPending,
    const bool trackedApplicationInactive,
    const bool applicationSlotFree) {
    return automaticLaunchPending
        || (trackedApplicationInactive && applicationSlotFree);
}

constexpr bool cloudWorkerPidIsStale(
    const bool processInformationAvailable,
    const bool expectedProgramObserved) {
    return !processInformationAvailable || !expectedProgramObserved;
}

// Horizon releases the Application resource slot asynchronously after the
// transient worker exits. Even when pm:info no longer exposes its PID, a new
// worker must wait until this short release window has elapsed.
constexpr bool cloudWorkerReleaseDelayElapsed(
    const std::uint64_t nowNs,
    const std::uint64_t launchNotBeforeNs) {
    return nowNs >= launchNotBeforeNs;
}

} // namespace nxsync
