#include "nxsync_installer/filesystem.hpp"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
namespace nxsync::installer {
bool ensureInstallDirectories(const std::string& sdRoot, std::string& error) {
    const char* directories[] = {
        "/switch", "/switch/NXSync", "/switch/.overlays",
        "/config", "/config/NXSync", "/atmosphere", "/atmosphere/contents",
        "/atmosphere/contents/010000000000000D",
        "/atmosphere/contents/4200000000004E58",
        "/atmosphere/contents/4200000000004E58/flags",
        "/atmosphere/contents/4200000000004E59",
    };
    for (const auto directory : directories) {
        const auto path = sdRoot + directory;
        if (mkdir(path.c_str(), 0777) == 0) continue;
        struct stat info{};
        if (errno == EEXIST && stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) continue;
        error = "Cannot create installer directory " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}
}
