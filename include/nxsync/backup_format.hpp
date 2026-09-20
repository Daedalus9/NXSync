#pragma once

#include <string>
#include <vector>

namespace nxsync {

constexpr const char* BackupSchema = "nxsync-backup";
constexpr unsigned BackupFormatVersion = 2;
constexpr const char* PayloadHashAlgorithm = "sha256-tree-v1";

struct BackupFormatIdentity {
    std::string schema;
    unsigned version{0};
    std::string revisionId;
    std::string parentRevisionId;
    std::string payloadHashAlgorithm;
    std::string payloadSha256;
    std::vector<std::string> parentRevisionIds;
};

enum class BackupFormatValidation {
    Valid,
    Legacy,
    UnsupportedVersion,
    InvalidIdentity,
};

BackupFormatValidation validateBackupFormatIdentity(
    const BackupFormatIdentity& identity);

const char* backupFormatValidationMessage(BackupFormatValidation validation);

} // namespace nxsync
