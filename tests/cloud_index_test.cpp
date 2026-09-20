#include "nxsync/cloud_index.hpp"

#include <cassert>
#include <string>

int main() {
    nxsync::CloudIndexEntry entry;
    entry.titleId = "0100A3D008C5C000";
    entry.titleName = "Pokemon \"Scarlet\"";
    entry.gameVersion = "4.0.0";
    entry.deviceId = "NS-OLED-ABC123";
    entry.profileName = "PlayerOne";
    entry.profileUid = std::string(32, 'A');
    entry.revisionId = std::string(64, 'b');
    entry.parentRevisionId = std::string(64, 'e');
    entry.parentRevisionIds = {
        entry.parentRevisionId,
        std::string(64, 'f')};
    entry.payloadSha256 = std::string(64, 'c');
    entry.archiveSha256 = std::string(64, 'd');
    entry.archiveRemotePath = "/NXSync/NS-OLED-ABC123/PlayerOne/0100A3D008C5C000/a.zip";
    entry.createdUtc = "2026-08-13T120000Z";
    entry.archiveSize = 123;
    entry.uncompressedBytes = 456;
    entry.fileCount = 2;

    std::string error;
    assert(nxsync::validateCloudIndexEntry(entry, error));
    assert(nxsync::makeCloudIndexEntryRemotePath(
        "NXSync", entry.titleId, entry.deviceId, entry.profileUid)
        == "/NXSync/_index/titles/0100A3D008C5C000/NS-OLED-ABC123_"
            + entry.profileUid + ".json");
    assert(nxsync::makeCloudRevisionRemotePath(
        "NXSync", entry.titleId, entry.revisionId)
        == "/NXSync/_index/revisions/0100A3D008C5C000/"
            + entry.revisionId + ".json");
    const std::string json = nxsync::serializeCloudIndexEntry(entry);
    assert(json.find("\"schema\": \"nxsync-cloud-head\"") != std::string::npos);
    assert(json.find("Pokemon \\\"Scarlet\\\"") != std::string::npos);
    assert(json.find("\"parent_revision_ids\"") != std::string::npos);

    nxsync::CloudIndexEntry parsed;
    assert(nxsync::parseCloudIndexEntry(json, parsed, error));
    assert(parsed.revisionId == entry.revisionId);
    assert(parsed.parentRevisionIds == entry.parentRevisionIds);
    assert(parsed.titleName == entry.titleName);

    // Existing single-parent documents remain readable after the DAG upgrade.
    std::string legacyJson = json;
    const std::string arrayLine = "  \"parent_revision_ids\": [\""
        + entry.parentRevisionIds[0] + "\", \""
        + entry.parentRevisionIds[1] + "\"],\n";
    const std::size_t arrayPosition = legacyJson.find(arrayLine);
    assert(arrayPosition != std::string::npos);
    legacyJson.erase(arrayPosition, arrayLine.size());
    nxsync::CloudIndexEntry legacyParsed;
    assert(nxsync::parseCloudIndexEntry(legacyJson, legacyParsed, error));
    assert(legacyParsed.parentRevisionIds.size() == 1);
    assert(legacyParsed.parentRevisionIds.front() == entry.parentRevisionId);

    entry.revisionId = "invalid";
    assert(!nxsync::validateCloudIndexEntry(entry, error));
    return 0;
}
