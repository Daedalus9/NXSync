#pragma once

#include "nxsync/device_identity.hpp"
#include "nxsync/save_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

struct BackupProgress {
    std::string stage;
    std::string currentPath;
    std::size_t filesProcessed{0};
    std::uint64_t bytesProcessed{0};
    std::size_t totalFiles{0};
    std::uint64_t totalBytes{0};
};

using BackupProgressCallback = void (*)(const BackupProgress& progress, void* context);

struct BackupResult {
    bool success{false};
    bool emptySave{false};
    bool lineageAnchorConsumed{false};
    Result mountResult{0};
    int systemError{0};
    std::string message;
    std::string archivePath;
    std::string sha256;
    std::string revisionId;
    std::string parentRevisionId;
    std::vector<std::string> parentRevisionIds;
    std::string payloadSha256;
    std::size_t fileCount{0};
    std::uint64_t uncompressedBytes{0};
};

BackupResult createLocalBackup(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    BackupProgressCallback progressCallback = nullptr,
    void* progressContext = nullptr);

} // namespace nxsync
