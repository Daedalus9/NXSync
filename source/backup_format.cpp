#include "nxsync/backup_format.hpp"
#include "nxsync/revision_parents.hpp"

#include <algorithm>
#include <cctype>

namespace nxsync {
namespace {

bool isSha256(const std::string& value) {
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

} // namespace

BackupFormatValidation validateBackupFormatIdentity(
    const BackupFormatIdentity& identity) {
    if (identity.schema != BackupSchema) {
        return BackupFormatValidation::Legacy;
    }
    if (identity.version != BackupFormatVersion) {
        return BackupFormatValidation::UnsupportedVersion;
    }
    if (!isSha256(identity.revisionId)
        || !validRevisionParents(
            identity.revisionId,
            identity.parentRevisionId,
            identity.parentRevisionIds)
        || identity.payloadHashAlgorithm != PayloadHashAlgorithm
        || !isSha256(identity.payloadSha256)) {
        return BackupFormatValidation::InvalidIdentity;
    }
    return BackupFormatValidation::Valid;
}

const char* backupFormatValidationMessage(const BackupFormatValidation validation) {
    switch (validation) {
        case BackupFormatValidation::Legacy:
            return "Legacy backup format is not supported";
        case BackupFormatValidation::UnsupportedVersion:
            return "Unsupported backup format version";
        case BackupFormatValidation::InvalidIdentity:
            return "Invalid v2 backup identity or hash";
        case BackupFormatValidation::Valid:
        default:
            return "Valid v2 backup";
    }
}

} // namespace nxsync
