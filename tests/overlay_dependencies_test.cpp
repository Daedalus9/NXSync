#include "nxsync/overlay_dependencies.hpp"

#include <cassert>

int main() {
    const nxsync::OverlayDependencyStatus ready{true, true, true};
    assert(ready.ready());
    assert(nxsync::missingOverlayDependencySummary(ready) == "none");

    const nxsync::OverlayDependencyStatus missingLoader{false, true, true};
    assert(!missingLoader.ready());
    assert(nxsync::missingOverlayDependencySummary(missingLoader)
        == "nx-ovlloader");

    const nxsync::OverlayDependencyStatus missingSeveral{true, false, false};
    assert(!missingSeveral.ready());
    assert(nxsync::missingOverlayDependencySummary(missingSeveral)
        == "nx-ovlloader boot flag, overlay menu");
    return 0;
}
