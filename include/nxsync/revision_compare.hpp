#pragma once

#include <string>
#include <vector>

namespace nxsync {

struct RevisionNode {
    std::string revisionId;
    std::string parentRevisionId;
    std::string payloadSha256;
    std::vector<std::string> parentRevisionIds;
};

std::vector<std::string> revisionParents(const RevisionNode& node);

enum class RevisionRelation {
    NoLocalRevision,
    SameRevision,
    SamePayload,
    CloudNewer,
    LocalNewer,
    Diverged,
    Unrelated,
    Invalid,
};

RevisionRelation compareRevisions(
    const RevisionNode& local,
    const RevisionNode& cloud,
    const std::vector<RevisionNode>& knownRevisions);

const char* revisionRelationLabel(RevisionRelation relation);

} // namespace nxsync
