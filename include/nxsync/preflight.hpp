#pragma once

#include "nxsync/cloud_index.hpp"
#include "nxsync/revision_compare.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace nxsync {

enum class PreflightOutcome {
    NoCloudRevision,
    Synchronized,
    LocalNewer,
    CloudUpdateAvailable,
    Conflict,
    InvalidCloudIndex,
};

struct PreflightDecision {
    PreflightOutcome outcome{PreflightOutcome::NoCloudRevision};
    bool hasSelectedHead{false};
    std::size_t selectedHeadIndex{0};
    std::size_t validHeads{0};
    std::size_t invalidHeads{0};
    std::string message;
};

PreflightDecision evaluatePreflight(
    const RevisionNode& local,
    const std::vector<CloudIndexEntry>& heads,
    const std::vector<RevisionNode>& history);

// Returns the cloud heads that represent genuinely distinct restore choices.
// A cloud copy with the same revision or payload as the current local save is
// not a useful alternative, and equivalent cloud payloads are shown once.
std::vector<std::size_t> cloudRestoreChoiceIndices(
    const RevisionNode& local,
    const std::vector<CloudIndexEntry>& heads);

const char* preflightOutcomeLabel(PreflightOutcome outcome);

} // namespace nxsync
