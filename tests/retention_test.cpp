#include "nxsync/retention.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void touch(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "test";
}

} // namespace

int main() {
    const std::string oldest = "2026-01-01T010101Z_000000000001.zip";
    const std::string middle = "2026-02-01T010101Z_000000000002.zip";
    const std::string newest = "2026-03-01T010101Z_000000000003.zip";
    const std::string future = "2040-01-01T010101Z_000000000004.zip";

    assert(nxsync::isNxsyncBackupFilename(oldest));
    assert(!nxsync::isNxsyncBackupFilename("backup.zip"));
    assert(!nxsync::isNxsyncBackupFilename(".2026-01-01T010101Z_000000000001.partial.zip"));
    assert(!nxsync::isNxsyncBackupFilename("2026-01-01T010101Z_00000000000X.zip"));

    const std::vector<std::string> names{
        newest, oldest, "notes.zip", middle, future, newest};
    const std::vector<std::string> disabled = nxsync::selectBackupsToPrune(
        names, 0, newest);
    assert(disabled.empty());

    const std::vector<std::string> fiveBackups{
        "2026-01-01T010101Z_000000000001.zip",
        "2026-01-02T010101Z_000000000002.zip",
        "2026-01-03T010101Z_000000000003.zip",
        "2026-01-04T010101Z_000000000004.zip",
        "2026-01-05T010101Z_000000000005.zip",
        "2026-01-06T010101Z_000000000006.zip",
        "2026-01-07T010101Z_000000000007.zip",
        "revision.json"};
    const std::vector<std::string> prunedToFive =
        nxsync::selectBackupsToPrune(
            fiveBackups,
            5,
            "2026-01-07T010101Z_000000000007.zip");
    assert(prunedToFive.size() == 2);
    assert(std::find(
        prunedToFive.begin(), prunedToFive.end(), "revision.json")
        == prunedToFive.end());

    const std::vector<std::string> selected = nxsync::selectBackupsToPrune(
        names, 2, oldest);
    assert(selected.size() == 2);
    assert(std::find(selected.begin(), selected.end(), oldest) == selected.end());
    assert(std::find(selected.begin(), selected.end(), future) == selected.end());
    assert(std::find(selected.begin(), selected.end(), newest) != selected.end());
    assert(std::find(selected.begin(), selected.end(), middle) != selected.end());

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "nxsync-retention-test";
    std::filesystem::create_directories(directory);
    for (const std::string& name : {oldest, middle, newest, future, std::string("notes.zip")}) {
        touch(directory / name);
    }
    const nxsync::RetentionResult result = nxsync::pruneLocalBackups(
        directory.string(), oldest, 2);
    assert(result.removed == 2);
    assert(result.failed == 0);
    assert(std::filesystem::exists(directory / oldest));
    assert(std::filesystem::exists(directory / future));
    assert(std::filesystem::exists(directory / "notes.zip"));
    assert(!std::filesystem::exists(directory / middle));
    assert(!std::filesystem::exists(directory / newest));

    std::filesystem::remove_all(directory);
    return 0;
}
