#include "nxsync/game_version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace nxsync {
namespace {

std::string trim(std::string value) {
    const auto notSpace = [](const unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool startsWithIgnoreCase(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size()
        && std::equal(prefix.begin(), prefix.end(), value.begin(), [](const char left, const char right) {
            return std::tolower(static_cast<unsigned char>(left))
                == std::tolower(static_cast<unsigned char>(right));
        });
}

bool parseVersion(std::string value, std::vector<std::uint32_t>& parts) {
    value = trim(std::move(value));
    const char* prefixes[] = {"version ", "ver. ", "ver "};
    for (const char* prefix : prefixes) {
        const std::string candidate(prefix);
        if (startsWithIgnoreCase(value, candidate)) {
            value = trim(value.substr(candidate.size()));
            break;
        }
    }
    if (value.size() > 1
        && (value.front() == 'v' || value.front() == 'V')
        && std::isdigit(static_cast<unsigned char>(value[1]))) {
        value.erase(value.begin());
    }
    if (value.empty()) {
        return false;
    }

    parts.clear();
    std::uint64_t current = 0;
    bool hasDigit = false;
    for (std::size_t index = 0; index <= value.size(); ++index) {
        const bool atEnd = index == value.size();
        const unsigned char ch = atEnd ? 0 : static_cast<unsigned char>(value[index]);
        if (!atEnd && std::isdigit(ch)) {
            hasDigit = true;
            current = current * 10 + static_cast<unsigned>(ch - '0');
            if (current > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            continue;
        }
        if (!hasDigit || (!atEnd && ch != '.') || parts.size() >= 8) {
            return false;
        }
        parts.push_back(static_cast<std::uint32_t>(current));
        current = 0;
        hasDigit = false;
    }
    while (parts.size() > 1 && parts.back() == 0) {
        parts.pop_back();
    }
    return !parts.empty();
}

} // namespace

GameVersionOrder compareGameVersions(
    const std::string& installedVersion,
    const std::string& backupVersion) {
    std::vector<std::uint32_t> installed;
    std::vector<std::uint32_t> backup;
    if (!parseVersion(installedVersion, installed)
        || !parseVersion(backupVersion, backup)) {
        return GameVersionOrder::Unknown;
    }
    const std::size_t count = std::max(installed.size(), backup.size());
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t left = index < installed.size() ? installed[index] : 0;
        const std::uint32_t right = index < backup.size() ? backup[index] : 0;
        if (left < right) {
            return GameVersionOrder::Older;
        }
        if (left > right) {
            return GameVersionOrder::Newer;
        }
    }
    return GameVersionOrder::Equal;
}

} // namespace nxsync
