#pragma once

#include <cstdint>
#include <string_view>

namespace nxsync {

constexpr bool restoreAllowsLaunch(bool success, bool destinationModified, bool recoverySucceeded) {
    return success || !destinationModified || recoverySucceeded;
}

constexpr bool restoreMatchesSelectedRevision(
    std::string_view selectedRevision, std::string_view selectedPayload,
    std::string_view archiveRevision, std::string_view archivePayload) {
    return selectedRevision.size() == 64 && selectedPayload.size() == 64
        && selectedRevision == archiveRevision && selectedPayload == archivePayload;
}

enum class LaunchReleaseDisposition {
    WaitingForResult,
    WaitingForWorkerExit,
    Ready,
};

constexpr LaunchReleaseDisposition launchReleaseDisposition(
    const bool finalResultAvailable,
    const std::uint64_t workerProcessId) {
    if (!finalResultAvailable) {
        return LaunchReleaseDisposition::WaitingForResult;
    }
    return workerProcessId == 0
        ? LaunchReleaseDisposition::Ready
        : LaunchReleaseDisposition::WaitingForWorkerExit;
}

} // namespace nxsync
