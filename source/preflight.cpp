#include "nxsync/preflight.hpp"

#include <optional>
#include <set>
#include <vector>

namespace nxsync {
namespace {

RevisionNode nodeFromHead(const CloudIndexEntry& head) {
    return {
        head.revisionId,
        head.parentRevisionId,
        head.payloadSha256,
        head.parentRevisionIds};
}

bool sameOrDescendant(
    const RevisionNode& older,
    const RevisionNode& candidate,
    const std::vector<RevisionNode>& history) {
    const RevisionRelation relation = compareRevisions(older, candidate, history);
    return relation == RevisionRelation::SameRevision
        || relation == RevisionRelation::SamePayload
        || relation == RevisionRelation::CloudNewer;
}

std::optional<std::size_t> uniqueMaximalHead(
    const std::vector<std::size_t>& candidates,
    const std::vector<CloudIndexEntry>& heads,
    const std::vector<RevisionNode>& history) {
    std::optional<std::size_t> selected;
    for (const std::size_t candidateIndex : candidates) {
        const RevisionNode candidate = nodeFromHead(heads[candidateIndex]);
        bool coversAll = true;
        for (const std::size_t otherIndex : candidates) {
            if (otherIndex == candidateIndex) continue;
            if (!sameOrDescendant(
                    nodeFromHead(heads[otherIndex]), candidate, history)) {
                coversAll = false;
                break;
            }
        }
        if (!coversAll) continue;
        if (selected.has_value()) {
            if (heads[*selected].payloadSha256 == heads[candidateIndex].payloadSha256) {
                continue;
            }
            return std::nullopt;
        }
        selected = candidateIndex;
    }
    return selected;
}

} // namespace

PreflightDecision evaluatePreflight(
    const RevisionNode& local,
    const std::vector<CloudIndexEntry>& heads,
    const std::vector<RevisionNode>& history) {
    PreflightDecision decision;
    if (heads.empty()) {
        decision.message = "No cloud revision available";
        return decision;
    }

    std::vector<std::size_t> validCandidates;
    for (std::size_t index = 0; index < heads.size(); ++index) {
        const RevisionRelation relation = compareRevisions(
            RevisionNode{}, nodeFromHead(heads[index]), history);
        if (relation == RevisionRelation::Invalid) {
            ++decision.invalidHeads;
            continue;
        }
        ++decision.validHeads;
        validCandidates.push_back(index);
    }

    if (decision.validHeads == 0) {
        decision.outcome = PreflightOutcome::InvalidCloudIndex;
        decision.message = "No valid cloud head";
        return decision;
    }

    // Device heads are intentionally immutable per console/profile. After a
    // logical merge, older device heads can therefore remain visible even
    // though one maximal revision descends from all of them. Compare the local
    // save with that maximal head only, otherwise an already resolved branch
    // would be reported as a new conflict.
    const std::optional<std::size_t> maximal = uniqueMaximalHead(
        validCandidates, heads, history);
    if (maximal.has_value()) {
        const RevisionRelation relation = compareRevisions(
            local, nodeFromHead(heads[*maximal]), history);
        switch (relation) {
            case RevisionRelation::SameRevision:
            case RevisionRelation::SamePayload:
                decision.outcome = PreflightOutcome::Synchronized;
                decision.message = "Local and cloud saves are synchronized";
                return decision;
            case RevisionRelation::LocalNewer:
                decision.outcome = PreflightOutcome::LocalNewer;
                decision.message = "The local save is the newest";
                return decision;
            case RevisionRelation::CloudNewer:
            case RevisionRelation::NoLocalRevision:
                decision.outcome = PreflightOutcome::CloudUpdateAvailable;
                decision.hasSelectedHead = true;
                decision.selectedHeadIndex = *maximal;
                decision.message = "A newer cloud revision is available";
                return decision;
            case RevisionRelation::Diverged:
            case RevisionRelation::Unrelated:
                decision.outcome = PreflightOutcome::Conflict;
                decision.hasSelectedHead = true;
                decision.selectedHeadIndex = *maximal;
                decision.message =
                    "Local and cloud histories diverged: choose which one to use";
                return decision;
            case RevisionRelation::Invalid:
                decision.outcome = PreflightOutcome::InvalidCloudIndex;
                decision.message = "Unable to determine the relationship between revisions";
                return decision;
        }
    }

    std::vector<std::size_t> cloudCandidates;
    bool synchronized = false;
    bool localNewer = false;
    bool conflict = false;
    for (const std::size_t index : validCandidates) {
        const RevisionRelation relation = compareRevisions(
            local, nodeFromHead(heads[index]), history);
        switch (relation) {
            case RevisionRelation::SameRevision:
            case RevisionRelation::SamePayload:
                synchronized = true;
                break;
            case RevisionRelation::LocalNewer:
                localNewer = true;
                break;
            case RevisionRelation::CloudNewer:
            case RevisionRelation::NoLocalRevision:
                cloudCandidates.push_back(index);
                break;
            case RevisionRelation::Diverged:
            case RevisionRelation::Unrelated:
                conflict = true;
                break;
            case RevisionRelation::Invalid:
                break;
        }
    }
    if (conflict) {
        decision.outcome = PreflightOutcome::Conflict;
        const std::optional<std::size_t> selected = uniqueMaximalHead(
            validCandidates, heads, history);
        if (selected.has_value()) {
            decision.hasSelectedHead = true;
            decision.selectedHeadIndex = *selected;
            decision.message =
                "Local and cloud histories diverged: choose which one to use";
        } else {
            decision.message =
                "Multiple cloud revisions conflict: no automatic selection";
        }
        return decision;
    }

    if (!cloudCandidates.empty()) {
        const std::optional<std::size_t> selected = uniqueMaximalHead(
            cloudCandidates, heads, history);
        if (!selected.has_value()) {
            decision.outcome = PreflightOutcome::Conflict;
            decision.message = "Multiple newer cloud revisions cannot be compared";
            return decision;
        }
        decision.outcome = PreflightOutcome::CloudUpdateAvailable;
        decision.hasSelectedHead = true;
        decision.selectedHeadIndex = *selected;
        decision.message = "A newer cloud revision is available";
        return decision;
    }

    if (localNewer) {
        decision.outcome = PreflightOutcome::LocalNewer;
        decision.message = "The local save is the newest";
    } else if (synchronized) {
        decision.outcome = PreflightOutcome::Synchronized;
        decision.message = "Local and cloud saves are synchronized";
    } else {
        decision.outcome = PreflightOutcome::InvalidCloudIndex;
        decision.message = "Unable to determine the relationship between revisions";
    }
    return decision;
}

std::vector<std::size_t> cloudRestoreChoiceIndices(
    const RevisionNode& local,
    const std::vector<CloudIndexEntry>& heads) {
    std::vector<std::size_t> choices;
    std::set<std::string> payloads;
    std::set<std::string> revisions;
    for (std::size_t index = 0; index < heads.size(); ++index) {
        const CloudIndexEntry& head = heads[index];
        const RevisionNode cloud = nodeFromHead(head);
        const RevisionRelation relation = compareRevisions(local, cloud, {});
        if (relation == RevisionRelation::Invalid
            || relation == RevisionRelation::SameRevision
            || relation == RevisionRelation::SamePayload
            || head.archiveRemotePath.empty()) {
            continue;
        }

        // The payload hash describes the actual files in the save. Different
        // revisions carrying the same payload are one user-facing choice.
        if (!head.payloadSha256.empty()) {
            if (!payloads.insert(head.payloadSha256).second) continue;
        } else if (!revisions.insert(head.revisionId).second) {
            continue;
        }
        choices.push_back(index);
    }
    return choices;
}

const char* preflightOutcomeLabel(const PreflightOutcome outcome) {
    switch (outcome) {
        case PreflightOutcome::NoCloudRevision: return "No cloud revision";
        case PreflightOutcome::Synchronized: return "Synchronized";
        case PreflightOutcome::LocalNewer: return "Local is newer";
        case PreflightOutcome::CloudUpdateAvailable: return "Newer cloud revision";
        case PreflightOutcome::Conflict: return "Conflict";
        case PreflightOutcome::InvalidCloudIndex:
        default: return "Invalid index";
    }
}

} // namespace nxsync
