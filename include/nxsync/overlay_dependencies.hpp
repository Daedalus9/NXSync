#pragma once

#include <string>
#include <sys/stat.h>

namespace nxsync {

constexpr const char* OverlayLoaderBinaryPath =
    "sdmc:/atmosphere/contents/420000000007E51A/exefs.nsp";
constexpr const char* OverlayLoaderBootFlagPath =
    "sdmc:/atmosphere/contents/420000000007E51A/flags/boot2.flag";
constexpr const char* OverlayMenuPath =
    "sdmc:/switch/.overlays/ovlmenu.ovl";

struct OverlayDependencyStatus {
    bool loaderBinary{false};
    bool loaderBootFlag{false};
    bool overlayMenu{false};

    bool ready() const {
        return loaderBinary && loaderBootFlag && overlayMenu;
    }
};

inline bool overlayDependencyFileExists(
    const char* path,
    const bool requireData) {
    struct stat info {};
    return stat(path, &info) == 0
        && S_ISREG(info.st_mode)
        && (!requireData || info.st_size > 0);
}

inline OverlayDependencyStatus detectOverlayDependencies() {
    return OverlayDependencyStatus{
        overlayDependencyFileExists(OverlayLoaderBinaryPath, true),
        overlayDependencyFileExists(OverlayLoaderBootFlagPath, false),
        overlayDependencyFileExists(OverlayMenuPath, true),
    };
}

inline std::string missingOverlayDependencySummary(
    const OverlayDependencyStatus& status) {
    std::string result;
    const auto append = [&result](const char* value) {
        if (!result.empty()) result += ", ";
        result += value;
    };
    if (!status.loaderBinary) append("nx-ovlloader");
    if (!status.loaderBootFlag) append("nx-ovlloader boot flag");
    if (!status.overlayMenu) append("overlay menu");
    return result.empty() ? "none" : result;
}

} // namespace nxsync
