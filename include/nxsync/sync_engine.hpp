#pragma once

#include "nxsync/cloud_index.hpp"
#include "nxsync/cloud_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

struct SyncSaveDescriptor {
    std::string remoteRoot;
    std::string deviceId;
    std::string profileName;
    std::string profileUid;
    std::string titleId;
    std::string titleName;
    std::string gameVersion;
    std::string saveDataId;
};

struct SyncRevisionDescriptor {
    std::string archivePath;
    std::string archiveSha256;
    std::string revisionId;
    std::string parentRevisionId;
    std::vector<std::string> parentRevisionIds;
    std::string payloadSha256;
    std::string createdUtc;
    std::uint64_t archiveSize{0};
    std::uint64_t uncompressedBytes{0};
    std::size_t fileCount{0};
    bool markLocalState{false};
};

struct SyncUploadPlan {
    bool valid{false};
    bool markLocalState{false};
    std::string error;
    PendingCloudOperation operation;
    CloudIndexEntry indexEntry;
    std::string revisionIndexRemotePath;
    std::string headIndexRemotePath;
};

struct SyncTransferResult {
    bool success{false};
    std::string message;
};

struct SyncDocumentResult : SyncTransferResult {
    std::string text;
};

class SyncCloudTransport {
public:
    virtual ~SyncCloudTransport() = default;

    virtual SyncTransferResult uploadArchive(
        const std::string& localPath,
        const std::string& remotePath,
        const std::string& sha256) = 0;
    virtual SyncTransferResult uploadDocument(
        const std::string& text,
        const std::string& remotePath) = 0;
    virtual SyncDocumentResult downloadDocument(
        const std::string& remotePath) = 0;
};

using SyncMarkLocalStateCallback = bool (*)(
    const std::string& remotePath,
    void* context,
    std::string& error);

struct SyncExecutionResult {
    bool success{false};
    bool archiveUploaded{false};
    bool indexPublished{false};
    bool localStateMarked{false};
    std::string message;
    std::string remotePath;
};

enum class PendingOperationMatch {
    Match,
    DifferentSave,
    StaleRevision,
    InvalidPlan,
};

class SyncEngine {
public:
    explicit SyncEngine(
        std::string queueRoot,
        std::string storageEnvironment = {});

    const std::string& queueRoot() const;
    const std::string& storageEnvironment() const;
    std::vector<PendingCloudOperation> pendingOperations() const;

    SyncUploadPlan planUpload(
        const SyncSaveDescriptor& save,
        const SyncRevisionDescriptor& revision) const;
    PendingOperationMatch matchPendingOperation(
        const PendingCloudOperation& operation,
        const SyncUploadPlan& plan) const;

    bool defer(
        const SyncUploadPlan& plan,
        const std::string& message,
        int& systemError) const;
    bool recordFailure(
        const PendingCloudOperation& operation,
        const std::string& message,
        int& systemError) const;
    SyncExecutionResult execute(
        const SyncUploadPlan& plan,
        SyncCloudTransport& transport,
        bool archiveAlreadyVerified,
        SyncMarkLocalStateCallback markLocalState = nullptr,
        void* markContext = nullptr,
        bool preserveMatchingDocuments = false) const;

private:
    std::string queueRoot_;
    std::string storageEnvironment_;
};

} // namespace nxsync
