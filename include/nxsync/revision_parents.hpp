#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace nxsync {

constexpr std::size_t MaximumRevisionParents = 16;

inline bool isRevisionHash(const std::string& value) {
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

inline std::vector<std::string> normalizedRevisionParents(
    const std::string& legacyParent,
    const std::vector<std::string>& parents) {
    std::vector<std::string> result;
    const auto append = [&](const std::string& parent) {
        if (!parent.empty()
            && std::find(result.begin(), result.end(), parent) == result.end()) {
            result.push_back(parent);
        }
    };
    append(legacyParent);
    for (const std::string& parent : parents) append(parent);
    return result;
}

inline bool validRevisionParents(
    const std::string& revisionId,
    const std::string& legacyParent,
    const std::vector<std::string>& parents) {
    const std::vector<std::string> normalized = normalizedRevisionParents(
        legacyParent, parents);
    if (normalized.size() > MaximumRevisionParents) return false;
    return std::all_of(
        normalized.begin(), normalized.end(), [&](const std::string& parent) {
            return isRevisionHash(parent) && parent != revisionId;
        });
}

inline std::string primaryRevisionParent(
    const std::string& legacyParent,
    const std::vector<std::string>& parents) {
    const std::vector<std::string> normalized = normalizedRevisionParents(
        legacyParent, parents);
    return normalized.empty() ? std::string() : normalized.front();
}

} // namespace nxsync
