#include "nxsync/backup_request_probe.hpp"
#include "nxsync/backup_request_queue.hpp"

#include <cassert>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    const std::string root = "/tmp/nxsync-backup-probe-test-"
        + std::to_string(static_cast<long long>(getpid()));
    auto summary = nxsync::probePendingBackupRequests(
        root.c_str(), "emummc", 100, 15'000);
    assert(summary.validCount == 0);
    assert(summary.systemError == 0);
    assert(summary.failureStage == nxsync::BackupRequestProbeStage::None);

    nxsync::PendingBackupRequest request;
    request.storageEnvironment = "emummc";
    request.titleId = "0100F9F00C696000";
    request.triggerEvent = "exit";
    request.eventSequence = 2;
    request.requestedMonotonicNs = 1'000;
    request.notBeforeMonotonicNs = 11'000;
    request.settleDelaySeconds = 2;
    int systemError = 0;
    assert(nxsync::enqueueBackupRequest(root, request, systemError));

    summary = nxsync::probePendingBackupRequests(
        root.c_str(), "emummc", 10'999, 15'000);
    assert(summary.validCount == 1);
    assert(!summary.hasReadyRequest);
    assert(summary.nextWaitNs == 1);
    assert(std::string(summary.latestTitleId.data()) == request.titleId);
    summary = nxsync::probePendingBackupRequests(
        root.c_str(), "emummc", 11'000, 15'000);
    assert(summary.hasReadyRequest);
    assert(summary.nextWaitNs == 0);
    summary = nxsync::probePendingBackupRequests(
        root.c_str(), "emummc", 500, 15'000);
    assert(summary.hasReadyRequest);

    nxsync::PendingBackupRequest sysRequest = request;
    sysRequest.storageEnvironment = "sysmmc";
    sysRequest.eventSequence = 4;
    assert(nxsync::enqueueBackupRequest(root, sysRequest, systemError));
    summary = nxsync::probePendingBackupRequests(
        root.c_str(), "sysmmc", 11'000, 15'000);
    assert(summary.validCount == 1);
    assert(summary.hasReadyRequest);

    {
        std::ofstream corrupt(root + "/emummc-corrupt.request");
        corrupt << "version=3\nstorage_environment=emummc\n";
    }
    summary = nxsync::probePendingBackupRequests(
        root.c_str(), "emummc", 11'000, 15'000);
    assert(summary.validCount == 1);
    assert(summary.invalidCount == 1);

    assert(unlink((root + "/emummc-corrupt.request").c_str()) == 0);
    assert(nxsync::completeBackupRequest(root, request, systemError));
    assert(nxsync::completeBackupRequest(root, sysRequest, systemError));
    assert(rmdir(root.c_str()) == 0);
    return 0;
}
