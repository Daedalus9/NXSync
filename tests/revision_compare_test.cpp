#include "nxsync/revision_compare.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

nxsync::RevisionNode node(char id, char payload, const std::string& parent = {}) {
    return {std::string(64, id), parent, std::string(64, payload)};
}

} // namespace

int main() {
    const auto root = node('a', '1');
    const auto cloud = node('b', '2', root.revisionId);
    const auto cloud2 = node('c', '3', cloud.revisionId);
    const auto localBranch = node('d', '4', root.revisionId);
    const std::vector<nxsync::RevisionNode> graph{root, cloud, cloud2, localBranch};

    assert(nxsync::compareRevisions(root, cloud2, graph)
        == nxsync::RevisionRelation::CloudNewer);
    assert(nxsync::compareRevisions(cloud2, root, graph)
        == nxsync::RevisionRelation::LocalNewer);
    assert(nxsync::compareRevisions(localBranch, cloud2, graph)
        == nxsync::RevisionRelation::Diverged);
    assert(nxsync::compareRevisions(root, root, graph)
        == nxsync::RevisionRelation::SameRevision);

    auto samePayload = node('e', '1');
    assert(nxsync::compareRevisions(root, samePayload, graph)
        == nxsync::RevisionRelation::SamePayload);

    nxsync::RevisionNode merge{
        std::string(64, '9'),
        cloud2.revisionId,
        std::string(64, '8'),
        {cloud2.revisionId, localBranch.revisionId}};
    auto mergedGraph = graph;
    mergedGraph.push_back(merge);
    assert(nxsync::revisionParents(merge).size() == 2);
    assert(nxsync::compareRevisions(localBranch, merge, mergedGraph)
        == nxsync::RevisionRelation::CloudNewer);
    assert(nxsync::compareRevisions(cloud2, merge, mergedGraph)
        == nxsync::RevisionRelation::CloudNewer);
    assert(nxsync::compareRevisions(merge, localBranch, mergedGraph)
        == nxsync::RevisionRelation::LocalNewer);

    auto invalidMerge = merge;
    invalidMerge.parentRevisionIds.push_back(invalidMerge.revisionId);
    assert(nxsync::compareRevisions(root, invalidMerge, mergedGraph)
        == nxsync::RevisionRelation::Invalid);
    auto unrelated = node('f', '9');
    assert(nxsync::compareRevisions(root, unrelated, graph)
        == nxsync::RevisionRelation::Unrelated);
    nxsync::RevisionNode noLocal;
    assert(nxsync::compareRevisions(noLocal, cloud, graph)
        == nxsync::RevisionRelation::NoLocalRevision);
    return 0;
}
