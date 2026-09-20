#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

constexpr unsigned BackupRequestVersion = 3;

struct PendingBackupRequest {
    unsigned version{BackupRequestVersion};
    std::string storageEnvironment;
    std::string titleId;
    std::string triggerEvent;
    std::uint64_t eventSequence{0};
    std::uint64_t requestedMonotonicNs{0};
    std::uint64_t notBeforeMonotonicNs{0};
    std::uint32_t settleDelaySeconds{0};
    std::size_t attemptCount{0};
    std::uint64_t lastAttemptMonotonicNs{0};
    std::string lastError;
};

bool validatePendingBackupRequest(
    const PendingBackupRequest& request,
    std::string& error);
std::string serializePendingBackupRequest(const PendingBackupRequest& request);
bool parsePendingBackupRequest(
    const std::string& text,
    PendingBackupRequest& request,
    std::string& error);

std::vector<PendingBackupRequest> loadPendingBackupRequests(
    const std::string& queueRoot,
    const std::string& storageEnvironment = {});
bool enqueueBackupRequest(
    const std::string& queueRoot,
    const PendingBackupRequest& request,
    int& systemError);
bool completeBackupRequest(
    const std::string& queueRoot,
    const PendingBackupRequest& request,
    int& systemError);
bool recordBackupRequestFailure(
    const std::string& queueRoot,
    PendingBackupRequest request,
    const std::string& message,
    std::uint64_t currentMonotonicNs,
    std::uint32_t retryDelaySeconds,
    int& systemError);
bool isBackupRequestReady(
    const PendingBackupRequest& request,
    std::uint64_t currentMonotonicNs);
std::uint64_t nextBackupRequestWaitDurationNs(
    const std::vector<PendingBackupRequest>& requests,
    std::uint64_t currentMonotonicNs,
    std::uint64_t maximumWaitNs);

} // namespace nxsync
