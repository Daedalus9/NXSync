#pragma once
#include <string>
namespace nxsync::installer {
// sdRoot is "sdmc:" on Switch and a temporary directory in host tests.
bool ensureInstallDirectories(const std::string& sdRoot, std::string& error);
}
