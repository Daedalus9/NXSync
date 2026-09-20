#include "nxsync/ultrahand_notification.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    nxsync::UltrahandNotification notification;
    notification.title = "NXSync \"test\"";
    notification.text = "Riga 1\nRiga 2\\fine";
    notification.fontSize = 60;
    notification.durationMs = 100;
    notification.priority = 25;
    const std::string json = nxsync::serializeUltrahandNotification(notification);
    assert(json.find("NXSync \\\"test\\\"") != std::string::npos);
    assert(json.find("Riga 1\\nRiga 2\\\\fine") != std::string::npos);
    assert(json.find("\"font_size\":48") != std::string::npos);
    assert(json.find("\"duration\":500") != std::string::npos);
    assert(json.find("\"show_time\":\"false\"") != std::string::npos);

    const std::string root = "/tmp/nxsync-notification-test-"
        + std::to_string(static_cast<unsigned long long>(getpid()));
    const std::string directory = root + "/nested/notifications";
    int systemError = 0;
    assert(nxsync::postUltrahandNotification(
        directory, "NX Sync!", notification, 42, systemError));
    const std::string finalPath = directory + "/NXSync-42.notify";
    std::ifstream input(finalPath, std::ios::binary);
    assert(input.good());
    const std::string written(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(written == json);
    assert(std::remove(finalPath.c_str()) == 0);
    assert(rmdir(directory.c_str()) == 0);
    assert(rmdir((root + "/nested").c_str()) == 0);
    assert(rmdir(root.c_str()) == 0);
    return 0;
}
