#pragma once

#include <string>

namespace nxsync {

enum class GameVersionOrder {
    Unknown,
    Older,
    Equal,
    Newer,
};

GameVersionOrder compareGameVersions(
    const std::string& installedVersion,
    const std::string& backupVersion);

} // namespace nxsync
