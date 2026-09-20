#include "nxsync/cloud_worker_policy.hpp"

#include <cassert>

int main() {
    assert(nxsync::cloudWorkerNetworkWaitBudgetMs(false) == 12'000);
    assert(nxsync::cloudWorkerNetworkWaitBudgetMs(true) == 2'000);
    assert(nxsync::CloudWorkerNetworkPollIntervalMs == 500);
    assert(nxsync::CloudTransferAttemptLimit == 3);
    assert(nxsync::cloudTransferRetryDelaySeconds(0) == 1);
    assert(nxsync::cloudTransferRetryDelaySeconds(1) == 2);
    assert(nxsync::cloudTransferRetryDelaySeconds(2) == 2);
    assert(nxsync::cloudHttpStatusIsTransient(423));
    assert(nxsync::cloudHttpStatusIsTransient(429));
    assert(nxsync::cloudHttpStatusIsTransient(500));
    assert(nxsync::cloudHttpStatusIsTransient(502));
    assert(nxsync::cloudHttpStatusIsTransient(503));
    assert(nxsync::cloudHttpStatusIsTransient(504));
    assert(!nxsync::cloudHttpStatusIsTransient(401));
    assert(!nxsync::cloudHttpStatusIsTransient(403));
    assert(!nxsync::cloudHttpStatusIsTransient(404));
    assert(!nxsync::cloudHttpStatusIsTransient(507));
    assert(nxsync::cloudHttpRetryDelaySeconds(423, 0) == 5);
    assert(nxsync::cloudHttpRetryDelaySeconds(423, 1) == 15);
    assert(nxsync::cloudHttpRetryDelaySeconds(503, 0) == 2);
    assert(nxsync::cloudHttpRetryDelaySeconds(503, 1) == 5);
    assert(!nxsync::cloudWorkerNeedsRetry(false, 0));
    assert(nxsync::cloudWorkerNeedsRetry(false, 1));
    assert(nxsync::cloudWorkerNeedsRetry(true, 0));
    assert(nxsync::cloudWorkerNeedsRetry(true, 3));
    assert(nxsync::cloudWorkerMayUseApplicationSlot(true, false, false));
    assert(nxsync::cloudWorkerMayUseApplicationSlot(false, true, true));
    assert(!nxsync::cloudWorkerMayUseApplicationSlot(false, true, false));
    assert(!nxsync::cloudWorkerMayUseApplicationSlot(false, false, true));
    assert(nxsync::cloudWorkerPidIsStale(false, false));
    assert(nxsync::cloudWorkerPidIsStale(true, false));
    assert(!nxsync::cloudWorkerPidIsStale(true, true));
    assert(nxsync::cloudWorkerReleaseDelayElapsed(0, 0));
    assert(!nxsync::cloudWorkerReleaseDelayElapsed(1'999'999'999ULL, 2'000'000'000ULL));
    assert(nxsync::cloudWorkerReleaseDelayElapsed(2'000'000'000ULL, 2'000'000'000ULL));
    assert(nxsync::cloudWorkerReleaseDelayElapsed(2'000'000'001ULL, 2'000'000'000ULL));
    return 0;
}
