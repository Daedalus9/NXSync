#include "nxsync/sync_engine.hpp"

#include "nxsync/remote_layout.hpp"
#include "nxsync/storage_environment.hpp"

#include <cerrno>
#include <string>
#include <utility>

namespace nxsync {
namespace {

std::string fileNameFromPath(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

bool sameSave(
    const PendingCloudOperation& operation,
    const PendingCloudOperation& planned) {
    return operation.titleId == planned.titleId
        && operation.saveDataId == planned.saveDataId
        && operation.profileUid == planned.profileUid
        && (operation.storageEnvironment.empty()
                ? "emummc"
                : operation.storageEnvironment)
            == planned.storageEnvironment;
}

} // namespace

SyncEngine::SyncEngine(
    std::string queueRoot,
    std::string storageEnvironment)
    : queueRoot_(std::move(queueRoot)),
      storageEnvironment_(std::move(storageEnvironment)) {
    if (storageEnvironment_.empty()) {
#ifdef __SWITCH__
        storageEnvironment_ = storageEnvironmentKey(
            detectCurrentStorageEnvironment());
#else
        storageEnvironment_ = "emummc";
#endif
    }
}

const std::string& SyncEngine::queueRoot() const {
    return queueRoot_;
}

const std::string& SyncEngine::storageEnvironment() const {
    return storageEnvironment_;
}

std::vector<PendingCloudOperation> SyncEngine::pendingOperations() const {
    return loadPendingCloudOperations(queueRoot_, storageEnvironment_);
}

SyncUploadPlan SyncEngine::planUpload(
    const SyncSaveDescriptor& save,
    const SyncRevisionDescriptor& revision) const {
    SyncUploadPlan plan;
    plan.operation.storageEnvironment = storageEnvironment_;
    plan.operation.revisionId = revision.revisionId;
    plan.operation.titleId = save.titleId;
    plan.operation.saveDataId = save.saveDataId;
    plan.operation.profileUid = save.profileUid;
    plan.operation.archivePath = revision.archivePath;
    plan.operation.remotePath = makeBackupRemotePath(
        save.remoteRoot,
        save.deviceId,
        save.profileUid,
        save.titleId,
        fileNameFromPath(revision.archivePath));
    plan.operation.archiveSha256 = revision.archiveSha256;

    plan.indexEntry.titleId = save.titleId;
    plan.indexEntry.titleName = save.titleName;
    plan.indexEntry.gameVersion = save.gameVersion;
    plan.indexEntry.deviceId = save.deviceId;
    plan.indexEntry.profileName = save.profileName;
    plan.indexEntry.profileUid = save.profileUid;
    plan.indexEntry.revisionId = revision.revisionId;
    plan.indexEntry.parentRevisionId = revision.parentRevisionId;
    plan.indexEntry.parentRevisionIds = revision.parentRevisionIds;
    plan.indexEntry.payloadSha256 = revision.payloadSha256;
    plan.indexEntry.archiveSha256 = revision.archiveSha256;
    plan.indexEntry.archiveRemotePath = plan.operation.remotePath;
    plan.indexEntry.createdUtc = revision.createdUtc;
    plan.indexEntry.archiveSize = revision.archiveSize;
    plan.indexEntry.uncompressedBytes = revision.uncompressedBytes;
    plan.indexEntry.fileCount = revision.fileCount;
    plan.markLocalState = revision.markLocalState;

    if (queueRoot_.empty()) {
        plan.error = "Cloud queue folder is not configured";
        return plan;
    }
    if (!validatePendingCloudOperation(plan.operation, plan.error)
        || !validateCloudIndexEntry(plan.indexEntry, plan.error)) {
        return plan;
    }
    plan.revisionIndexRemotePath = makeCloudRevisionRemotePath(
        save.remoteRoot,
        save.titleId,
        revision.revisionId);
    plan.headIndexRemotePath = makeCloudIndexEntryRemotePath(
        save.remoteRoot,
        save.titleId,
        save.deviceId,
        save.profileUid);
    plan.valid = true;
    return plan;
}

PendingOperationMatch SyncEngine::matchPendingOperation(
    const PendingCloudOperation& operation,
    const SyncUploadPlan& plan) const {
    if (!plan.valid) return PendingOperationMatch::InvalidPlan;
    if (!sameSave(operation, plan.operation)) {
        return PendingOperationMatch::DifferentSave;
    }
    if (operation.revisionId != plan.operation.revisionId
        || operation.archivePath != plan.operation.archivePath
        || operation.archiveSha256 != plan.operation.archiveSha256
        || operation.remotePath != plan.operation.remotePath) {
        return PendingOperationMatch::StaleRevision;
    }
    return PendingOperationMatch::Match;
}

bool SyncEngine::defer(
    const SyncUploadPlan& plan,
    const std::string& message,
    int& systemError) const {
    if (!plan.valid) {
        systemError = EINVAL;
        return false;
    }
    if (!enqueueCloudOperation(queueRoot_, plan.operation, systemError)) {
        return false;
    }
    return recordCloudOperationFailure(
        queueRoot_,
        plan.operation,
        message,
        systemError);
}

bool SyncEngine::recordFailure(
    const PendingCloudOperation& operation,
    const std::string& message,
    int& systemError) const {
    return recordCloudOperationFailure(
        queueRoot_,
        operation,
        message,
        systemError);
}

SyncExecutionResult SyncEngine::execute(
    const SyncUploadPlan& plan,
    SyncCloudTransport& transport,
    const bool archiveAlreadyVerified,
    const SyncMarkLocalStateCallback markLocalState,
    void* const markContext,
    const bool preserveMatchingDocuments) const {
    SyncExecutionResult result;
    if (!plan.valid) {
        result.message = plan.error.empty() ? "Invalid synchronization plan" : plan.error;
        return result;
    }
    result.remotePath = plan.operation.remotePath;
    int queueError = 0;
    if (!enqueueCloudOperation(queueRoot_, plan.operation, queueError)) {
        result.message = "Unable to record the operation in the cloud queue (errno "
            + std::to_string(queueError) + ")";
        return result;
    }

    if (!archiveAlreadyVerified) {
        const SyncTransferResult archive = transport.uploadArchive(
            plan.operation.archivePath,
            plan.operation.remotePath,
            plan.operation.archiveSha256);
        if (!archive.success) {
            result.message = archive.message;
            recordCloudOperationFailure(
                queueRoot_, plan.operation, result.message, queueError);
            return result;
        }
        result.archiveUploaded = true;
    }

    const std::string document = serializeCloudIndexEntry(plan.indexEntry);
    const auto publishDocument = [&](const std::string& remotePath) {
        if (preserveMatchingDocuments) {
            const SyncDocumentResult current = transport.downloadDocument(remotePath);
            if (current.success && current.text == document) {
                return SyncTransferResult{true, "Cloud index is already up to date"};
            }
        }
        return transport.uploadDocument(document, remotePath);
    };
    const SyncTransferResult revision = publishDocument(
        plan.revisionIndexRemotePath);
    if (!revision.success) {
        result.message = "ZIP verified, but global index was not updated: "
            + revision.message;
        recordCloudOperationFailure(
            queueRoot_, plan.operation, result.message, queueError);
        return result;
    }
    const SyncTransferResult head = publishDocument(plan.headIndexRemotePath);
    if (!head.success) {
        result.message = "ZIP verified, but global index was not updated: "
            + head.message;
        recordCloudOperationFailure(
            queueRoot_, plan.operation, result.message, queueError);
        return result;
    }
    result.indexPublished = true;

    if (plan.indexEntry.archiveRemotePath.empty()) {
        result.message = "Cloud path is missing after publication";
        recordCloudOperationFailure(
            queueRoot_, plan.operation, result.message, queueError);
        return result;
    }
    if (plan.markLocalState) {
        if (markLocalState == nullptr) {
            result.message = "Local status callback is not configured";
            recordCloudOperationFailure(
                queueRoot_, plan.operation, result.message, queueError);
            return result;
        }
        std::string markError;
        if (!markLocalState(plan.operation.remotePath, markContext, markError)) {
            result.message = markError.empty()
                ? "Upload verified, but local status was not updated"
                : markError;
            recordCloudOperationFailure(
                queueRoot_, plan.operation, result.message, queueError);
            return result;
        }
        result.localStateMarked = true;
    }

    if (!completeCloudOperation(queueRoot_, plan.operation, queueError)) {
        result.message = "Cloud verified, but local queue was not finalized (errno "
            + std::to_string(queueError) + ")";
        return result;
    }
    result.success = true;
    result.message = "ZIP and global index verified";
    return result;
}

} // namespace nxsync
