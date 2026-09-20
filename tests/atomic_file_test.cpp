#include "nxsync/atomic_file.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "nxsync-atomic-file-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path target = root / "protocol.status";

    {
        std::ofstream old(target, std::ios::binary);
        old << "old";
    }
    {
        std::ofstream stale(target.string() + ".new", std::ios::binary);
        stale << "partial";
    }

    int systemError = -1;
    assert(nxsync::writeTextFileAtomic(
        target.string(), "version=1\nstate=complete\n", systemError));
    assert(systemError == 0);
    assert(readAll(target) == "version=1\nstate=complete\n");
    // An abandoned legacy temporary is never mistaken for a committed record.
    assert(readAll(target.string() + ".new") == "partial");

    assert(nxsync::writeTextFileAtomic(
        target.string(), "second\n", systemError));
    assert(readAll(target) == "second\n");

    std::filesystem::remove_all(root);
    return 0;
}
