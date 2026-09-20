#include "nxsync_installer/filesystem.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("nxsync-installer-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    std::string error;
    assert(nxsync::installer::ensureInstallDirectories(root.string(), error));
    std::ofstream payload(root / "atmosphere/contents/010000000000000D/exefs.nsp");
    assert(payload); payload << "new override"; payload.close();
    assert(nxsync::installer::ensureInstallDirectories(root.string(), error));
    assert(std::filesystem::is_directory(root / "atmosphere/contents/4200000000004E58/flags"));
    const auto collision = root / "collision";
    std::filesystem::create_directory(collision);
    std::ofstream(collision / "switch") << "existing user file";
    assert(!nxsync::installer::ensureInstallDirectories(collision.string(), error));
    assert(std::filesystem::is_regular_file(collision / "switch"));
    std::filesystem::remove_all(root);
}
