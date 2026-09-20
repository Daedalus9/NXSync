#include "nxsync/revision_lineage.hpp"

#include <cassert>
#include <string>

int main() {
    const std::string previous(64, 'a');
    nxsync::RestoreLineageAnchor imported{
        true,
        std::string(64, 'b'),
        std::string(64, 'c')};

    assert(nxsync::validateRestoreLineageAnchor(imported));
    assert(nxsync::selectBackupParentRevision(true, previous, imported)
        == imported.revisionId);
    imported.parentRevisionIds = {
        imported.revisionId,
        std::string(64, 'd')};
    const auto mergeParents = nxsync::selectBackupParentRevisions(
        true, previous, imported);
    assert(mergeParents.size() == 2);
    assert(mergeParents[0] == imported.revisionId);
    assert(mergeParents[1] == std::string(64, 'd'));
    const nxsync::RevisionNode restored = nxsync::restoredRevisionNode(imported);
    assert(restored.revisionId == imported.revisionId);
    assert(restored.payloadSha256 == imported.payloadSha256);
    assert(restored.parentRevisionId.empty());
    assert(restored.parentRevisionIds.empty());
    assert(nxsync::compareRevisions(
        restored,
        nxsync::RevisionNode{
            imported.revisionId,
            std::string(64, 'e'),
            imported.payloadSha256,
            {std::string(64, 'e')}},
        {}) == nxsync::RevisionRelation::SameRevision);

    imported.valid = false;
    imported.parentRevisionIds.clear();
    assert(nxsync::selectBackupParentRevision(true, previous, imported)
        == previous);
    assert(nxsync::selectBackupParentRevision(false, previous, imported).empty());

    imported.valid = true;
    imported.revisionId = "invalid";
    assert(!nxsync::validateRestoreLineageAnchor(imported));
    assert(nxsync::restoredRevisionNode(imported).revisionId.empty());
    assert(nxsync::selectBackupParentRevision(true, previous, imported)
        == previous);
    return 0;
}
