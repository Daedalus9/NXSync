#include "nxsync/backup_request_queue.hpp"

#include <cassert>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    nxsync::PendingBackupRequest request;
    request.storageEnvironment = "emummc";
    request.titleId = "0100A3D008C5C000";
    request.triggerEvent = "exit";
    request.eventSequence = 2;
    request.requestedMonotonicNs = 1'000;
    request.notBeforeMonotonicNs = 11'000;
    request.settleDelaySeconds = 0;
    request.attemptCount = 0;
    std::string error;
    assert(nxsync::validatePendingBackupRequest(request, error));
    nxsync::PendingBackupRequest parsed;
    assert(nxsync::parsePendingBackupRequest(
        nxsync::serializePendingBackupRequest(request), parsed, error));
    assert(parsed.titleId == request.titleId);
    assert(parsed.storageEnvironment == "emummc");
    assert(!nxsync::isBackupRequestReady(parsed, 10'999));
    assert(nxsync::isBackupRequestReady(parsed, 11'000));
    assert(nxsync::isBackupRequestReady(parsed, 500));

    const std::string queueRoot = "/tmp/nxsync-backup-request-test-"
        + std::to_string(static_cast<long long>(getpid()));
    int systemError = 0;
    assert(nxsync::enqueueBackupRequest(queueRoot, request, systemError));
    auto pending = nxsync::loadPendingBackupRequests(queueRoot);
    assert(pending.size() == 1);
    assert(pending.front().eventSequence == 2);

    request.eventSequence = 4;
    request.triggerEvent = "crash";
    request.requestedMonotonicNs = 20'000;
    request.notBeforeMonotonicNs = 30'000;
    assert(nxsync::enqueueBackupRequest(queueRoot, request, systemError));
    pending = nxsync::loadPendingBackupRequests(queueRoot);
    assert(pending.size() == 1);
    assert(pending.front().eventSequence == 4);
    assert(pending.front().triggerEvent == "crash");

    nxsync::PendingBackupRequest sysRequest = request;
    sysRequest.storageEnvironment = "sysmmc";
    sysRequest.eventSequence = 6;
    assert(nxsync::enqueueBackupRequest(queueRoot, sysRequest, systemError));
    assert(nxsync::loadPendingBackupRequests(queueRoot).size() == 2);
    assert(nxsync::loadPendingBackupRequests(queueRoot, "emummc").size() == 1);
    assert(nxsync::loadPendingBackupRequests(queueRoot, "sysmmc").size() == 1);

    assert(nxsync::recordBackupRequestFailure(
        queueRoot,
        request,
        "save data busy",
        40'000,
        60,
        systemError));
    pending = nxsync::loadPendingBackupRequests(queueRoot, "emummc");
    assert(pending.size() == 1);
    assert(pending.front().version == nxsync::BackupRequestVersion);
    assert(pending.front().attemptCount == 1);
    assert(pending.front().lastError == "save data busy");
    assert(pending.front().notBeforeMonotonicNs == 60'000'040'000ULL);

    const std::string legacy =
        "version=1\ntitle_id=0100A3D008C5C000\ntrigger_event=exit\n"
        "event_sequence=2\nrequested_monotonic_ns=1000\n"
        "not_before_monotonic_ns=11000\nsettle_delay_seconds=10\n";
    assert(nxsync::parsePendingBackupRequest(legacy, parsed, error));
    assert(parsed.version == 1);
    assert(parsed.attemptCount == 0);
    {
        std::ofstream output(queueRoot + "/" + parsed.titleId + ".request");
        output << legacy;
    }
    assert(nxsync::loadPendingBackupRequests(queueRoot, "emummc").size() == 2);
    assert(nxsync::loadPendingBackupRequests(queueRoot, "sysmmc").size() == 1);
    assert(nxsync::completeBackupRequest(queueRoot, parsed, systemError));

    assert(nxsync::completeBackupRequest(queueRoot, request, systemError));
    assert(nxsync::completeBackupRequest(queueRoot, sysRequest, systemError));
    assert(nxsync::loadPendingBackupRequests(queueRoot).empty());

    nxsync::PendingBackupRequest immediate = request;
    immediate.notBeforeMonotonicNs = immediate.requestedMonotonicNs;
    assert(nxsync::nextBackupRequestWaitDurationNs(
        {immediate}, immediate.requestedMonotonicNs, 15'000'000'000ULL) == 0);
    nxsync::PendingBackupRequest delayed = request;
    delayed.notBeforeMonotonicNs = delayed.requestedMonotonicNs
        + 1'000'000'000ULL;
    assert(nxsync::nextBackupRequestWaitDurationNs(
        {delayed}, delayed.requestedMonotonicNs, 15'000'000'000ULL)
        == 1'000'000'000ULL);
    assert(nxsync::nextBackupRequestWaitDurationNs(
        {}, delayed.requestedMonotonicNs, 15'000'000'000ULL)
        == 15'000'000'000ULL);
    assert(rmdir(queueRoot.c_str()) == 0);
    return 0;
}
