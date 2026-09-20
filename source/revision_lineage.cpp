#include "nxsync/revision_lineage.hpp"
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

bool validateRestoreLineageAnchor(const RestoreLineageAnchor& anchor) {
    if (!anchor.valid
        || !isSha256(anchor.revisionId)
        || !isSha256(anchor.payloadSha256)) {
        return false;
    }
    const std::vector<std::string> parents = normalizedRevisionParents(
        anchor.revisionId, anchor.parentRevisionIds);
    return !parents.empty()
        && parents.size() <= MaximumRevisionParents
        && std::all_of(parents.begin(), parents.end(), isSha256);
}

RevisionNode restoredRevisionNode(const RestoreLineageAnchor& anchor) {
    if (!validateRestoreLineageAnchor(anchor)) return {};
    return {
        anchor.revisionId,
        std::string(),
        anchor.payloadSha256,
        {}};
}

std::string selectBackupParentRevision(
    const bool previousRecordValid,
    const std::string& previousRevisionId,
    const RestoreLineageAnchor& restoreAnchor) {
    const std::vector<std::string> parents = selectBackupParentRevisions(
        previousRecordValid, previousRevisionId, restoreAnchor);
    return parents.empty() ? std::string() : parents.front();
}

std::vector<std::string> selectBackupParentRevisions(
    const bool previousRecordValid,
    const std::string& previousRevisionId,
    const RestoreLineageAnchor& restoreAnchor) {
    if (validateRestoreLineageAnchor(restoreAnchor)) {
        return normalizedRevisionParents(
            restoreAnchor.revisionId,
            restoreAnchor.parentRevisionIds);
    }
    return previousRecordValid && isSha256(previousRevisionId)
        ? std::vector<std::string>{previousRevisionId}
        : std::vector<std::string>{};
}

} // namespace nxsync
