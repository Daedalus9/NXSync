#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

constexpr unsigned CloudOperationVersion = 2;

struct PendingCloudOperation {
    unsigned version{CloudOperationVersion};
    std::string storageEnvironment;
    std::string revisionId;
    std::string titleId;
    std::string saveDataId;
    std::string profileUid;
    std::string archivePath;
    std::string remotePath;
    std::string archiveSha256;
    std::size_t attemptCount{0};
    std::string lastError;
};

bool validatePendingCloudOperation(
    const PendingCloudOperation& operation,
    std::string& error);
std::string serializePendingCloudOperation(const PendingCloudOperation& operation);
bool parsePendingCloudOperation(
    const std::string& text,
    PendingCloudOperation& operation,
    std::string& error);

std::vector<PendingCloudOperation> loadPendingCloudOperations(
    const std::string& queueRoot,
    const std::string& storageEnvironment = {});
bool enqueueCloudOperation(
    const std::string& queueRoot,
    const PendingCloudOperation& operation,
    int& systemError);
bool recordCloudOperationFailure(
    const std::string& queueRoot,
    PendingCloudOperation operation,
    const std::string& message,
    int& systemError);
bool completeCloudOperation(
    const std::string& queueRoot,
    const PendingCloudOperation& operation,
    int& systemError);

} // namespace nxsync
