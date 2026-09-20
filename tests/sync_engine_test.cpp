#include "nxsync/sync_engine.hpp"

#include <cassert>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

class MockTransport final : public nxsync::SyncCloudTransport {
public:
    bool failArchive{false};
    bool failFirstDocument{false};
    std::string downloadedDocument;
    std::vector<std::string> archives;
    std::vector<std::string> documents;
    std::vector<std::string> downloads;

    nxsync::SyncTransferResult uploadArchive(
        const std::string&,
        const std::string& remotePath,
        const std::string&) override {
        archives.push_back(remotePath);
        return failArchive
            ? nxsync::SyncTransferResult{false, "archive offline"}
            : nxsync::SyncTransferResult{true, "archive ok"};
    }

    nxsync::SyncTransferResult uploadDocument(
        const std::string&,
        const std::string& remotePath) override {
        documents.push_back(remotePath);
        if (failFirstDocument && documents.size() == 1) {
            return {false, "index offline"};
        }
        return {true, "index ok"};
    }

    nxsync::SyncDocumentResult downloadDocument(
        const std::string& remotePath) override {
        downloads.push_back(remotePath);
        nxsync::SyncDocumentResult result;
        result.success = !downloadedDocument.empty();
        result.message = result.success ? "download ok" : "not found";
        result.text = downloadedDocument;
        return result;
    }
};

bool markLocal(const std::string& remotePath, void* context, std::string& error) {
    auto* calls = static_cast<std::size_t*>(context);
    ++*calls;
    error.clear();
    return remotePath.find("backup.zip") != std::string::npos;
}

nxsync::SyncSaveDescriptor saveDescriptor() {
    nxsync::SyncSaveDescriptor save;
    save.remoteRoot = "/NXSync";
    save.deviceId = "NS-OLED-ABC123";
    save.profileName = "PlayerOne";
    save.profileUid = std::string(32, 'A');
    save.titleId = "0100A3D008C5C000";
    save.titleName = "Pokemon Scarlatto";
    save.gameVersion = "4.0.0";
    save.saveDataId = "0000000000001234";
    return save;
}

nxsync::SyncRevisionDescriptor revisionDescriptor() {
    nxsync::SyncRevisionDescriptor revision;
    revision.archivePath = "sdmc:/switch/NXSync/backups/backup.zip";
    revision.archiveSha256 = std::string(64, 'B');
    revision.revisionId = std::string(64, 'C');
    revision.parentRevisionId = std::string(64, 'E');
    revision.parentRevisionIds = {
        revision.parentRevisionId,
        std::string(64, 'F')};
    revision.payloadSha256 = std::string(64, 'D');
    revision.createdUtc = "2026-08-13T12:00Z";
    revision.archiveSize = 123;
    revision.uncompressedBytes = 456;
    revision.fileCount = 3;
    revision.markLocalState = true;
    return revision;
}

} // namespace

int main() {
    const std::string queueRoot = "/tmp/nxsync-engine-test-"
        + std::to_string(static_cast<long long>(getpid()));
    const nxsync::SyncEngine engine(queueRoot);
    const nxsync::SyncUploadPlan plan = engine.planUpload(
        saveDescriptor(),
        revisionDescriptor());
    assert(plan.valid);
    assert(plan.indexEntry.parentRevisionIds
        == revisionDescriptor().parentRevisionIds);
    assert(plan.operation.remotePath
        == "/NXSync/NS-OLED-ABC123/profile-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/0100A3D008C5C000/backup.zip");
    assert(plan.revisionIndexRemotePath.find("/_index/revisions/") != std::string::npos);
    assert(plan.headIndexRemotePath.find("/_index/titles/") != std::string::npos);

    nxsync::PendingCloudOperation other = plan.operation;
    other.titleId = "0100000000000000";
    assert(engine.matchPendingOperation(other, plan)
        == nxsync::PendingOperationMatch::DifferentSave);
    nxsync::PendingCloudOperation stale = plan.operation;
    stale.revisionId = std::string(64, 'E');
    assert(engine.matchPendingOperation(stale, plan)
        == nxsync::PendingOperationMatch::StaleRevision);
    assert(engine.matchPendingOperation(plan.operation, plan)
        == nxsync::PendingOperationMatch::Match);

    MockTransport failing;
    failing.failFirstDocument = true;
    std::size_t markCalls = 0;
    nxsync::SyncExecutionResult result = engine.execute(
        plan,
        failing,
        false,
        markLocal,
        &markCalls);
    assert(!result.success);
    assert(result.archiveUploaded);
    assert(!result.indexPublished);
    assert(markCalls == 0);
    auto pending = engine.pendingOperations();
    assert(pending.size() == 1);
    assert(pending.front().attemptCount == 1);

    MockTransport retry;
    result = engine.execute(plan, retry, true, markLocal, &markCalls);
    assert(result.success);
    assert(!result.archiveUploaded);
    assert(result.indexPublished);
    assert(result.localStateMarked);
    assert(retry.archives.empty());
    assert(retry.documents.size() == 2);
    assert(markCalls == 1);
    assert(engine.pendingOperations().empty());

    MockTransport unchanged;
    unchanged.downloadedDocument = nxsync::serializeCloudIndexEntry(plan.indexEntry);
    result = engine.execute(plan, unchanged, true, markLocal, &markCalls, true);
    assert(result.success);
    assert(unchanged.downloads.size() == 2);
    assert(unchanged.documents.empty());
    assert(markCalls == 2);
    assert(engine.pendingOperations().empty());
    assert(rmdir(queueRoot.c_str()) == 0);
    return 0;
}
