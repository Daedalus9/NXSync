#include "nxsync/cloud_queue.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    nxsync::PendingCloudOperation operation;
    operation.storageEnvironment = "emummc";
    operation.revisionId = std::string(64, 'a');
    operation.titleId = "0100A3D008C5C000";
    operation.saveDataId = "0000000000001234";
    operation.profileUid = std::string(32, 'B');
    operation.archivePath = "sdmc:/switch/NXSync/backups/a=b.zip";
    operation.remotePath = "/NXSync/a.zip";
    operation.archiveSha256 = std::string(64, 'c');
    operation.attemptCount = 2;
    operation.lastError = "network\nunavailable";
    std::string error;
    assert(nxsync::validatePendingCloudOperation(operation, error));
    const std::string serialized = nxsync::serializePendingCloudOperation(operation);
    nxsync::PendingCloudOperation parsed;
    assert(nxsync::parsePendingCloudOperation(serialized, parsed, error));
    assert(parsed.archivePath == operation.archivePath);
    assert(parsed.lastError == operation.lastError);
    assert(parsed.attemptCount == 2);
    assert(parsed.storageEnvironment == "emummc");
    parsed.remotePath = "relative";
    assert(!nxsync::validatePendingCloudOperation(parsed, error));

    const std::string queueRoot = "/tmp/nxsync-cloud-queue-test-"
        + std::to_string(static_cast<long long>(getpid()));
    int systemError = 0;
    assert(nxsync::enqueueCloudOperation(queueRoot, operation, systemError));
    auto pending = nxsync::loadPendingCloudOperations(queueRoot);
    assert(pending.size() == 1);
    assert(pending.front().revisionId == operation.revisionId);

    nxsync::PendingCloudOperation sysOperation = operation;
    sysOperation.storageEnvironment = "sysmmc";
    assert(nxsync::enqueueCloudOperation(queueRoot, sysOperation, systemError));
    assert(nxsync::loadPendingCloudOperations(queueRoot).size() == 2);
    assert(nxsync::loadPendingCloudOperations(queueRoot, "emummc").size() == 1);
    assert(nxsync::loadPendingCloudOperations(queueRoot, "sysmmc").size() == 1);

    nxsync::PendingCloudOperation legacy = operation;
    legacy.version = 1;
    legacy.storageEnvironment.clear();
    legacy.revisionId = std::string(64, 'f');
    {
        std::ofstream output(queueRoot + "/" + legacy.revisionId + ".queue");
        output << nxsync::serializePendingCloudOperation(legacy);
    }
    assert(nxsync::loadPendingCloudOperations(queueRoot, "emummc").size() == 2);
    assert(nxsync::loadPendingCloudOperations(queueRoot, "sysmmc").size() == 1);
    assert(nxsync::completeCloudOperation(queueRoot, legacy, systemError));

    assert(nxsync::recordCloudOperationFailure(
        queueRoot,
        operation,
        "offline",
        systemError));
    pending = nxsync::loadPendingCloudOperations(queueRoot);
    assert(pending.size() == 2);
    assert(pending.front().attemptCount == 3);
    assert(pending.front().lastError == "offline");

    nxsync::PendingCloudOperation newer = operation;
    newer.revisionId = std::string(64, 'd');
    newer.archiveSha256 = std::string(64, 'e');
    newer.archivePath = "sdmc:/switch/NXSync/backups/new.zip";
    newer.remotePath = "/NXSync/new.zip";
    newer.attemptCount = 0;
    newer.lastError.clear();
    assert(nxsync::enqueueCloudOperation(queueRoot, newer, systemError));
    pending = nxsync::loadPendingCloudOperations(queueRoot, "emummc");
    assert(pending.size() == 1);
    assert(pending.front().revisionId == newer.revisionId);

    assert(nxsync::completeCloudOperation(
        queueRoot,
        newer,
        systemError));
    assert(nxsync::completeCloudOperation(
        queueRoot,
        sysOperation,
        systemError));
    assert(nxsync::loadPendingCloudOperations(queueRoot).empty());
    assert(rmdir(queueRoot.c_str()) == 0);
    return 0;
}
