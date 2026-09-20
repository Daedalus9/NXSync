#include "nxsync/overlay_catalog.hpp"

#include <cassert>
#include <string>

int main() {
    nxsync::OverlayCatalog catalog;
    catalog.generatedUnix = 1'786'660'000;
    catalog.entries.push_back(nxsync::OverlayCatalogEntry{
        "00112233445566778899AABBCCDDEEFF",
        "PlayerOne=uno\\due",
        "0100A3D008C5C000",
        "Pokémon Scarlet\nVersion 4"});

    const std::string serialized = nxsync::serializeOverlayCatalog(catalog);
    nxsync::OverlayCatalog parsed;
    std::string error;
    assert(nxsync::parseOverlayCatalog(serialized, parsed, error));
    assert(parsed.version == nxsync::OverlayCatalogVersion);
    assert(parsed.generatedUnix == catalog.generatedUnix);
    assert(parsed.entries.size() == 1);
    assert(parsed.entries.front().profileUid == catalog.entries.front().profileUid);

    std::string profileUid;
    assert(nxsync::findUniqueProfileUidForTitle(
        parsed, "0100A3D008C5C000", profileUid));
    assert(profileUid == "00112233445566778899AABBCCDDEEFF");
    assert(!nxsync::findUniqueProfileUidForTitle(
        parsed, "0100000000000000", profileUid));

    parsed.entries.push_back(nxsync::OverlayCatalogEntry{
        "FFEEDDCCBBAA99887766554433221100",
        "Second profile",
        "0100A3D008C5C000",
        "Example game"});
    assert(!nxsync::findUniqueProfileUidForTitle(
        parsed, "0100A3D008C5C000", profileUid));
    assert(profileUid.empty());
    assert(parsed.entries.front().profileName == catalog.entries.front().profileName);
    assert(parsed.entries.front().titleId == catalog.entries.front().titleId);
    assert(parsed.entries.front().titleName == catalog.entries.front().titleName);

    assert(!nxsync::parseOverlayCatalog(
        "version=1\ngenerated_unix=1\nentry_count=1\n",
        parsed,
        error));
    assert(!error.empty());
    return 0;
}
