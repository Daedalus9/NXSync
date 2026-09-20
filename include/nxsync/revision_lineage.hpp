#pragma once

#include "nxsync/revision_compare.hpp"

#include <string>
#include <vector>

namespace nxsync {

struct RestoreLineageAnchor {
    bool valid{false};
    std::string revisionId;
    std::string payloadSha256;
    std::vector<std::string> parentRevisionIds;
};

bool validateRestoreLineageAnchor(const RestoreLineageAnchor& anchor);
// The anchor's parent list describes the parents of the *next* local backup.
// It is not the ancestry of the already restored revision.  Preflight must
// therefore compare a parent-less node and obtain its real ancestry from the
// cloud revision graph.
RevisionNode restoredRevisionNode(const RestoreLineageAnchor& anchor);
std::string selectBackupParentRevision(
    bool previousRecordValid,
    const std::string& previousRevisionId,
    const RestoreLineageAnchor& restoreAnchor);
std::vector<std::string> selectBackupParentRevisions(
    bool previousRecordValid,
    const std::string& previousRevisionId,
    const RestoreLineageAnchor& restoreAnchor);

} // namespace nxsync
