#include "nxsync/backup_format.hpp"

#include <cassert>
#include <string>

int main() {
    const std::string hash(64, 'a');
    nxsync::BackupFormatIdentity identity{
        nxsync::BackupSchema,
        nxsync::BackupFormatVersion,
        hash,
        std::string(),
        nxsync::PayloadHashAlgorithm,
        hash};
    assert(nxsync::validateBackupFormatIdentity(identity)
        == nxsync::BackupFormatValidation::Valid);

    identity.schema.clear();
    assert(nxsync::validateBackupFormatIdentity(identity)
        == nxsync::BackupFormatValidation::Legacy);
    identity.schema = nxsync::BackupSchema;

    identity.version = 1;
    assert(nxsync::validateBackupFormatIdentity(identity)
        == nxsync::BackupFormatValidation::UnsupportedVersion);
    identity.version = nxsync::BackupFormatVersion;

    identity.parentRevisionId = "not-a-sha256";
    assert(nxsync::validateBackupFormatIdentity(identity)
        == nxsync::BackupFormatValidation::InvalidIdentity);
    identity.parentRevisionId = std::string(64, 'F');
    identity.parentRevisionIds = {
        identity.parentRevisionId,
        std::string(64, 'E')};
    assert(nxsync::validateBackupFormatIdentity(identity)
        == nxsync::BackupFormatValidation::Valid);
    identity.parentRevisionIds.push_back(identity.revisionId);
    assert(nxsync::validateBackupFormatIdentity(identity)
        == nxsync::BackupFormatValidation::InvalidIdentity);
    return 0;
}
