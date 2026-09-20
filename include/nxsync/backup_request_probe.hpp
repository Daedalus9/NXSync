#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nxsync {

enum class BackupRequestProbeStage {
    None,
    OpenFileSystem,
    OpenDirectory,
    ReadDirectory,
    CloseDirectory,
    BuildPath,
    OpenRequest,
    GetRequestSize,
    ReadRequest,
    CloseRequest,
};

const char* backupRequestProbeStageName(BackupRequestProbeStage stage);

// Allocation-free summary used by the memory-constrained resident sysmodule.
// The transient worker continues to use the complete request deserializer.
struct BackupRequestProbeSummary {
    std::size_t validCount{0};
    std::size_t invalidCount{0};
    bool hasReadyRequest{false};
    std::uint64_t nextWaitNs{0};
    std::uint64_t latestEventSequence{0};
    std::array<char, 17> latestTitleId{};
    int systemError{0};
    std::uint32_t nativeResult{0};
    BackupRequestProbeStage failureStage{BackupRequestProbeStage::None};
};

BackupRequestProbeSummary probePendingBackupRequests(
    const char* queueRoot,
    const char* storageEnvironment,
    std::uint64_t currentMonotonicNs,
    std::uint64_t maximumWaitNs);

} // namespace nxsync
