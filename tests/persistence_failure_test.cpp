#include "nxsync/atomic_file.hpp"
#include "nxsync/cloud_queue.hpp"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
enum class Fault { None, RenameIo, NoReplace, PublishAndRollback };
Fault fault = Fault::None;
std::string failedRevision;
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    const std::string src(from), dst(to);
    if (!failedRevision.empty() && src.find(failedRevision) != std::string::npos) {
        errno = EIO; return -1;
    }
    if (fault == Fault::RenameIo) { errno = EIO; return -1; }
    if (fault == Fault::NoReplace || fault == Fault::PublishAndRollback) {
        if (std::filesystem::exists(dst)) { errno = EEXIST; return -1; }
        if (fault == Fault::PublishAndRollback && dst.find(".bak") == std::string::npos) {
            errno = EIO; return -1;
        }
    }
    return __real_rename(from, to);
}

int main() {
    const auto root = std::filesystem::temp_directory_path()
        / ("nxsync-persistence-failure-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    const std::string path = (root / "status").string();
    int error = 0;
    std::string text;
    assert(nxsync::writeTextFileAtomic(path, "old", error));
    fault = Fault::RenameIo;
    assert(!nxsync::writeTextFileAtomic(path, "new", error));
    assert(nxsync::readTextFileRecoverable(path, text, error) && text == "old");
    fault = Fault::NoReplace;
    assert(nxsync::writeTextFileAtomic(path, "new", error));
    assert(nxsync::readTextFileRecoverable(path, text, error) && text == "new");
    fault = Fault::PublishAndRollback;
    assert(!nxsync::writeTextFileAtomic(path, "third", error));
    assert(!std::filesystem::exists(path));
    assert(nxsync::readTextFileRecoverable(path, text, error) && text == "new");
    fault = Fault::None;
    assert(nxsync::writeTextFileAtomic(path, "recovered", error));
    assert(nxsync::readTextFileRecoverable(path, text, error) && text == "recovered");
    assert(nxsync::removeTextFileRecoverable(path, error));
    assert(!nxsync::readTextFileRecoverable(path, text, error));

    nxsync::PendingCloudOperation a;
    a.storageEnvironment = "emummc";
    a.revisionId = std::string(64, 'f'); // previous can sort AFTER successor
    a.titleId = "0100000000000001";
    a.saveDataId = "0000000000000001";
    a.profileUid = std::string(32, '1');
    a.archivePath = "/backups/a.zip";
    a.remotePath = "/NXSync/a.zip";
    a.archiveSha256 = std::string(64, 'c');
    const auto queue = (root / "queue").string();
    assert(nxsync::enqueueCloudOperation(queue, a, error));
    auto b = a;
    b.revisionId = std::string(64, 'a');
    failedRevision = b.revisionId;
    assert(!nxsync::enqueueCloudOperation(queue, b, error));
    auto pending = nxsync::loadPendingCloudOperations(queue);
    assert(pending.size() == 1 && pending[0].revisionId == a.revisionId);
    failedRevision.clear();
    assert(nxsync::enqueueCloudOperation(queue, b, error));
    pending = nxsync::loadPendingCloudOperations(queue);
    assert(pending.size() == 1 && pending[0].revisionId == b.revisionId);
    const auto record = queue + "/emummc-" + b.revisionId + ".queue";
    std::filesystem::rename(record, record + ".bak");
    pending = nxsync::loadPendingCloudOperations(queue);
    assert(pending.size() == 1 && pending[0].revisionId == b.revisionId);
    assert(nxsync::completeCloudOperation(queue, b, error));
    assert(nxsync::loadPendingCloudOperations(queue).empty());
    std::filesystem::remove_all(root);
}
