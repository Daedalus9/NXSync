#include "nxsync/revision_compare.hpp"
#include "nxsync/revision_parents.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>

namespace nxsync {
namespace {

std::set<std::string> ancestors(
    const RevisionNode& start,
    const std::map<std::string, RevisionNode>& nodes) {
    std::set<std::string> result;
    std::vector<std::string> pending{start.revisionId};
    for (std::size_t cursor = 0;
         cursor < pending.size() && cursor < 4096;
         ++cursor) {
        const std::string& current = pending[cursor];
        if (!isRevisionHash(current) || !result.insert(current).second) continue;
        const auto found = nodes.find(current);
        if (found == nodes.end()) continue;
        for (const std::string& parent : revisionParents(found->second)) {
            if (result.count(parent) == 0) pending.push_back(parent);
        }
    }
    return result;
}

} // namespace

std::vector<std::string> revisionParents(const RevisionNode& node) {
    return normalizedRevisionParents(
        node.parentRevisionId, node.parentRevisionIds);
}

RevisionRelation compareRevisions(
    const RevisionNode& local,
    const RevisionNode& cloud,
    const std::vector<RevisionNode>& knownRevisions) {
    if (!isRevisionHash(cloud.revisionId)
        || !isRevisionHash(cloud.payloadSha256)
        || !validRevisionParents(
            cloud.revisionId,
            cloud.parentRevisionId,
            cloud.parentRevisionIds)) {
        return RevisionRelation::Invalid;
    }
    if (local.revisionId.empty()) {
        return RevisionRelation::NoLocalRevision;
    }
    if (!isRevisionHash(local.revisionId)
        || !isRevisionHash(local.payloadSha256)
        || !validRevisionParents(
            local.revisionId,
            local.parentRevisionId,
            local.parentRevisionIds)) {
        return RevisionRelation::Invalid;
    }
    if (local.revisionId == cloud.revisionId) {
        return RevisionRelation::SameRevision;
    }
    if (local.payloadSha256 == cloud.payloadSha256) {
        return RevisionRelation::SamePayload;
    }

    std::map<std::string, RevisionNode> nodes;
    for (const RevisionNode& node : knownRevisions) {
        if (isRevisionHash(node.revisionId)
            && validRevisionParents(
                node.revisionId,
                node.parentRevisionId,
                node.parentRevisionIds)) {
            nodes[node.revisionId] = node;
        }
    }
    nodes[local.revisionId] = local;
    nodes[cloud.revisionId] = cloud;
    const std::set<std::string> localAncestors = ancestors(local, nodes);
    const std::set<std::string> cloudAncestors = ancestors(cloud, nodes);
    if (cloudAncestors.count(local.revisionId) != 0) {
        return RevisionRelation::CloudNewer;
    }
    if (localAncestors.count(cloud.revisionId) != 0) {
        return RevisionRelation::LocalNewer;
    }
    std::vector<std::string> common;
    std::set_intersection(
        localAncestors.begin(), localAncestors.end(),
        cloudAncestors.begin(), cloudAncestors.end(),
        std::back_inserter(common));
    return common.empty()
        ? RevisionRelation::Unrelated
        : RevisionRelation::Diverged;
}

const char* revisionRelationLabel(const RevisionRelation relation) {
    switch (relation) {
        case RevisionRelation::NoLocalRevision: return "Cloud only";
        case RevisionRelation::SameRevision: return "Synchronized";
        case RevisionRelation::SamePayload: return "Identical content";
        case RevisionRelation::CloudNewer: return "Cloud is newer";
        case RevisionRelation::LocalNewer: return "Local is newer";
        case RevisionRelation::Diverged: return "Conflict";
        case RevisionRelation::Unrelated: return "Different origin";
        case RevisionRelation::Invalid:
        default: return "Invalid index";
    }
}

} // namespace nxsync
