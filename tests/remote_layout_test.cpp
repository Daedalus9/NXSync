#include "nxsync/remote_layout.hpp"
#include "nxsync/nextcloud_paths.hpp"

#include <cassert>
#include <string>

int main() {
    using nxsync::makeBackupRemotePath;
    using nxsync::makeDeviceRemoteRoot;
    using nxsync::normalizeRemoteRoot;
    using nxsync::sanitizePathSegment;

    const std::string hash(64, 'a'), wrongHash(64, 'b');
    const std::string archive = "/NXSync/device/profile/title/20260917_" + hash + ".zip";
    assert(nxsync::downloadHashMatches(archive, hash, hash));
    assert(nxsync::downloadHashMatches(archive, hash, ""));
    assert(nxsync::downloadHashMatches(archive, hash, std::string(64, 'A')));
    assert(nxsync::downloadHashMatches("date_" + hash.substr(0, 12) + ".zip", hash, ""));
    // A self-consistent server response must not replace the archive requested.
    assert(!nxsync::downloadHashMatches(archive, wrongHash, wrongHash));
    assert(!nxsync::downloadHashMatches(archive, hash, wrongHash));
    assert(!nxsync::downloadHashMatches(archive, hash, "malformed"));
    assert(!nxsync::downloadHashMatches("renamed.zip", hash, hash));
    assert(!nxsync::downloadHashMatches("date_" + hash.substr(0, 11) + ".zip", hash, hash));
    assert(!nxsync::downloadHashMatches(archive + ".extra", hash, hash));
    assert(!nxsync::downloadHashMatches("date_" + hash + "a.zip", hash, hash));

    assert(nxsync::isValidNextcloudUrl("https://cloud.example.com"));
    assert(nxsync::isValidNextcloudUrl(
        "HTTPS://cloud.example.com/remote.php/dav/files/user"));
    assert(!nxsync::isValidNextcloudUrl("http://cloud.example.com"));
    assert(!nxsync::isValidNextcloudUrl("https://"));
    assert(!nxsync::isValidNextcloudUrl("https://cloud.example.com/a b"));
    assert(!nxsync::isValidNextcloudUrl("https://cloud.example.com/#fragment"));

    assert(
        nxsync::makeNextcloudDavBaseUrl(
            "https://cloud.example.com/nextcloud/",
            "mario rossi")
        == "https://cloud.example.com/nextcloud/remote.php/dav/files/mario%20rossi");
    assert(
        nxsync::makeNextcloudDavBaseUrl(
            "https://cloud.example.com/remote.php/dav/files/user/",
            "ignored")
        == "https://cloud.example.com/remote.php/dav/files/user");
    assert(
        nxsync::makeNextcloudResourceUrl(
            "https://cloud.example.com/remote.php/dav/files/user",
            "/NXSync/NS OLED/file #1.zip")
        == "https://cloud.example.com/remote.php/dav/files/user/"
           "NXSync/NS%20OLED/file%20%231.zip");

    assert(nxsync::isMutableCloudIndexPath(
        "/NXSync/_index/titles/0100152000022000/device.json"));
    assert(nxsync::isMutableCloudIndexPath(
        "/Backups/NXSync/_index/titles/0100152000022000"));
    assert(!nxsync::isMutableCloudIndexPath(
        "/NXSync/_index/revisions/0100152000022000/revision.json"));
    assert(!nxsync::isMutableCloudIndexPath(
        "/NXSync/NS-OLED/profile/0100152000022000/backup.zip"));
    assert(
        nxsync::makeFreshNextcloudResourceUrl(
            "https://cloud.example.com/remote.php/dav/files/user",
            "/NXSync/_index/titles/0100152000022000/device.json",
            123456)
        == "https://cloud.example.com/remote.php/dav/files/user/"
           "NXSync/_index/titles/0100152000022000/device.json?nxsync_cb=123456");
    assert(
        nxsync::makeFreshNextcloudResourceUrl(
            "https://cloud.example.com/remote.php/dav/files/user",
            "/NXSync/_index/revisions/0100152000022000/revision.json",
            123456)
        == "https://cloud.example.com/remote.php/dav/files/user/"
           "NXSync/_index/revisions/0100152000022000/revision.json");

    assert(sanitizePathSegment("PlayerOne") == "PlayerOne");
    assert(sanitizePathSegment("Profile / test") == "Profile-test");
    assert(sanitizePathSegment("..", true).empty());

    assert(normalizeRemoteRoot("NXSync") == "/NXSync");
    assert(normalizeRemoteRoot("/Backups/Switches/") == "/Backups/Switches");
    assert(normalizeRemoteRoot("///") == "/NXSync");

    assert(
        makeDeviceRemoteRoot("NXSync", "NS-OLED-A1B2C3")
        == "/NXSync/NS-OLED-A1B2C3");

    assert(
        makeBackupRemotePath(
            "NXSync",
            "NS OLED A1B2C3",
            std::string(32, 'a'),
            "0100ABCDEF123000",
            "2026-08-12T183000Z_ab12cd34.zip")
        == "/NXSync/NS-OLED-A1B2C3/profile-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/0100ABCDEF123000/"
           "2026-08-12T183000Z_ab12cd34.zip");

    assert(
        makeBackupRemotePath("", "", "", "", "")
        .empty());

    const auto first = nxsync::makeProfileBackupFolder(std::string(32, 'A'));
    const auto second = nxsync::makeProfileBackupFolder(std::string(32, 'B'));
    assert(first != second);
    assert(nxsync::makeProfileBackupFolder("A B").empty());
    assert(nxsync::isProfileBackupDirectory("/NXSync/device/" + first + "/0100000000000001", std::string(32, 'a')));
    assert(!nxsync::isProfileBackupDirectory("/NXSync/device/" + first + "/0100000000000001", std::string(32, 'B')));
    assert(!nxsync::isProfileBackupDirectory("/NXSync/device/Player/0100000000000001", std::string(32, 'A')));

    return 0;
}
