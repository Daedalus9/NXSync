#include "nxsync/game_version.hpp"

#include <cassert>

int main() {
    using nxsync::GameVersionOrder;
    using nxsync::compareGameVersions;

    assert(compareGameVersions("2.0", "4.0.0") == GameVersionOrder::Older);
    assert(compareGameVersions("4.0.0", "4.0") == GameVersionOrder::Equal);
    assert(compareGameVersions("10.0", "2.0") == GameVersionOrder::Newer);
    assert(compareGameVersions("v4.1.0", "Ver. 4.0.0") == GameVersionOrder::Newer);
    assert(compareGameVersions("release-4", "4.0") == GameVersionOrder::Unknown);
    assert(compareGameVersions("", "4.0") == GameVersionOrder::Unknown);
    return 0;
}
