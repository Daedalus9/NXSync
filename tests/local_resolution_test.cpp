#include "nxsync/local_resolution.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    nxsync::PendingLocalResolution resolution;
    resolution.sequence = 77;
    resolution.storageEnvironment = "emummc";
    resolution.titleId = "0100152000022000";
    resolution.profileUid = "00112233445566778899AABBCCDDEEFF";
    resolution.parentRevisionId = std::string(64, 'a');
    resolution.parentPayloadSha256 = std::string(64, 'b');
    resolution.parentRevisionIds = {
        resolution.parentRevisionId,
        std::string(64, 'c')};

    std::string error;
    assert(nxsync::validatePendingLocalResolution(resolution, error));
    nxsync::PendingLocalResolution parsed;
    assert(nxsync::parsePendingLocalResolution(
        nxsync::serializePendingLocalResolution(resolution), parsed, error));
    assert(parsed.sequence == resolution.sequence);
    assert(parsed.storageEnvironment == "emummc");
    assert(parsed.parentRevisionId == resolution.parentRevisionId);
    assert(parsed.parentRevisionIds == resolution.parentRevisionIds);

    const std::string path = "local-resolution-test.pending";
    int systemError = 0;
    assert(nxsync::writePendingLocalResolutionAtomic(
        path, resolution, systemError));
    nxsync::PendingLocalResolution loaded;
    assert(nxsync::loadPendingLocalResolution(path, loaded, error));
    assert(!nxsync::completePendingLocalResolution(path, 78, systemError));
    assert(nxsync::completePendingLocalResolution(path, 77, systemError));

    resolution.storageEnvironment = "unknown";
    assert(!nxsync::validatePendingLocalResolution(resolution, error));
    std::remove(path.c_str());
    std::remove((path + ".new").c_str());
    return 0;
}
