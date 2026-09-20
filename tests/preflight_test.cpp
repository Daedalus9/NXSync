#include "nxsync/preflight.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

nxsync::RevisionNode node(char id, char payload, const std::string& parent = {}) {
    return {std::string(64, id), parent, std::string(64, payload)};
}

nxsync::CloudIndexEntry head(const nxsync::RevisionNode& node, const char* device) {
    nxsync::CloudIndexEntry entry;
    entry.revisionId = node.revisionId;
    entry.parentRevisionId = node.parentRevisionId;
    entry.parentRevisionIds = node.parentRevisionIds;
    entry.payloadSha256 = node.payloadSha256;
    entry.deviceId = device;
    return entry;
}

} // namespace

int main() {
    const auto root = node('a', '1');
    const auto newer = node('b', '2', root.revisionId);
    const auto newest = node('c', '3', newer.revisionId);
    const auto branch = node('d', '4', root.revisionId);
    const auto unrelated = node('e', '5');
    const std::vector<nxsync::RevisionNode> history{
        root, newer, newest, branch, unrelated};

    auto decision = nxsync::evaluatePreflight(root, {}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::NoCloudRevision);

    decision = nxsync::evaluatePreflight(root, {head(root, "A")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::Synchronized);

    // The launch fast path must resolve equal and direct parent/child heads
    // without downloading the complete remote revision history.
    decision = nxsync::evaluatePreflight(root, {head(root, "A")}, {});
    assert(decision.outcome == nxsync::PreflightOutcome::Synchronized);

    decision = nxsync::evaluatePreflight(root, {head(newer, "A")}, {});
    assert(decision.outcome == nxsync::PreflightOutcome::CloudUpdateAvailable);
    assert(decision.hasSelectedHead);

    decision = nxsync::evaluatePreflight(newer, {head(root, "A")}, {});
    assert(decision.outcome == nxsync::PreflightOutcome::LocalNewer);

    nxsync::RevisionNode merge{
        std::string(64, '9'),
        newest.revisionId,
        branch.payloadSha256,
        {newest.revisionId, branch.revisionId}};
    auto mergeHistory = history;
    mergeHistory.push_back(merge);
    decision = nxsync::evaluatePreflight(
        branch, {head(merge, "Switch 1")}, mergeHistory);
    assert(decision.outcome == nxsync::PreflightOutcome::Synchronized);
    decision = nxsync::evaluatePreflight(
        newest, {head(merge, "Switch 2")}, mergeHistory);
    assert(decision.outcome == nxsync::PreflightOutcome::CloudUpdateAvailable);
    decision = nxsync::evaluatePreflight(
        branch,
        {head(merge, "Switch 1"), head(newest, "Switch 2")},
        mergeHistory);
    assert(decision.outcome == nxsync::PreflightOutcome::Synchronized);

    // An indirect relationship remains ambiguous without history and must
    // therefore trigger the slower full-history pass.
    decision = nxsync::evaluatePreflight(root, {head(newest, "A")}, {});
    assert(decision.outcome == nxsync::PreflightOutcome::Conflict);

    decision = nxsync::evaluatePreflight(newest, {head(root, "A")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::LocalNewer);

    decision = nxsync::evaluatePreflight(
        root, {head(newer, "A"), head(newest, "B")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::CloudUpdateAvailable);
    assert(decision.hasSelectedHead);
    assert(decision.selectedHeadIndex == 1);

    decision = nxsync::evaluatePreflight(
        root, {head(newer, "A"), head(branch, "B")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::Conflict);
    assert(!decision.hasSelectedHead);

    decision = nxsync::evaluatePreflight(
        unrelated, {head(newest, "A")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::Conflict);
    assert(decision.hasSelectedHead);
    assert(decision.selectedHeadIndex == 0);

    decision = nxsync::evaluatePreflight(
        branch, {head(newer, "A"), head(newest, "B")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::Conflict);
    assert(decision.hasSelectedHead);
    assert(decision.selectedHeadIndex == 1);

    nxsync::RevisionNode noLocal;
    decision = nxsync::evaluatePreflight(noLocal, {head(newest, "A")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::CloudUpdateAvailable);

    decision = nxsync::evaluatePreflight(
        noLocal, {head(newest, "A"), head(unrelated, "B")}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::Conflict);

    // A head matching the local payload must not be presented as a separate
    // restore choice. Equal cloud payloads are also collapsed into one card.
    auto localCopy = head(root, "Switch 1");
    localCopy.archiveRemotePath = "/switch-1/root.zip";
    auto remoteBranch = head(branch, "Switch 2");
    remoteBranch.archiveRemotePath = "/switch-2/branch.zip";
    auto duplicateBranch = remoteBranch;
    duplicateBranch.revisionId = std::string(64, 'f');
    duplicateBranch.archiveRemotePath = "/switch-3/branch-copy.zip";
    const auto choices = nxsync::cloudRestoreChoiceIndices(
        root, {localCopy, remoteBranch, duplicateBranch});
    assert(choices.size() == 1);
    assert(choices[0] == 1);

    // Choosing the local save resolves the single remote branch by making the
    // next local revision descend from the discarded cloud head. Both cloud
    // head documents then point to comparable revisions, so the conflict must
    // not be offered again.
    const auto resolvedLocal = node('9', '6', branch.revisionId);
    auto resolvedHistory = history;
    resolvedHistory.push_back(resolvedLocal);
    decision = nxsync::evaluatePreflight(
        resolvedLocal,
        {head(resolvedLocal, "Switch 1"), head(branch, "Switch 2")},
        resolvedHistory);
    assert(decision.outcome == nxsync::PreflightOutcome::Synchronized);

    const auto noLocalChoices = nxsync::cloudRestoreChoiceIndices(
        noLocal, {localCopy, remoteBranch});
    assert(noLocalChoices.size() == 2);

    nxsync::CloudIndexEntry invalid;
    decision = nxsync::evaluatePreflight(root, {invalid}, history);
    assert(decision.outcome == nxsync::PreflightOutcome::InvalidCloudIndex);
    return 0;
}
