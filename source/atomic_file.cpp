#include "nxsync/atomic_file.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace nxsync {
namespace {
bool syncDirectory(const std::string& path, int& systemError) {
#ifdef __SWITCH__
    const auto separator = path.find(':');
    if (separator == std::string::npos
        || R_FAILED(fsdevCommitDevice(path.substr(0, separator).c_str()))) {
        systemError = EIO;
        return false;
    }
#else
    const auto slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "."
        : (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) { systemError = errno; return false; }
    const bool ok = fsync(fd) == 0;
    systemError = ok ? 0 : errno;
    close(fd);
    if (!ok) return false;
#endif
    return true;
}
bool readOne(const std::string& path, std::string& text, int& error,
             const std::size_t maximum) {
    FILE* input = std::fopen(path.c_str(), "rb");
    if (!input) { error = errno; return false; }
    text.clear();
    char buffer[4096];
    bool ok = true;
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), input)) {
        if (count > maximum - text.size()) { error = EFBIG; ok = false; break; }
        text.append(buffer, count);
    }
    if (std::ferror(input)) { error = EIO; ok = false; }
    std::fclose(input);
    if (ok) error = 0;
    return ok;
}
} // namespace

bool readTextFileRecoverable(const std::string& path, std::string& text,
                            int& systemError, const std::size_t maximumBytes) {
    if (readOne(path, text, systemError, maximumBytes)) return true;
    if (systemError != ENOENT) return false;
    return readOne(path + ".bak", text, systemError, maximumBytes);
}

bool writeTextFileAtomic(const std::string& path, const std::string& text,
                         int& systemError) {
    systemError = 0;
    const std::string backup = path + ".bak";
    struct stat info{};
    if (stat(path.c_str(), &info) == 0 && !S_ISREG(info.st_mode)) {
        systemError = EINVAL;
        return false;
    }
    // Recover the old committed generation before starting another operation.
    if (stat(path.c_str(), &info) != 0 && errno == ENOENT
        && stat(backup.c_str(), &info) == 0) {
        if (std::rename(backup.c_str(), path.c_str()) != 0) {
            systemError = errno;
            return false;
        }
        if (!syncDirectory(path, systemError)) return false;
    }
    static std::atomic<unsigned> counter{0};
#ifdef __SWITCH__
    const auto writer = armGetSystemTick();
#else
    const auto writer = getpid();
#endif
    const std::string temporary = path + ".new-" + std::to_string(writer)
        + "-" + std::to_string(++counter);
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { systemError = errno; return false; }
    FILE* output = fdopen(fd, "wb");
    if (!output) { systemError = errno; close(fd); std::remove(temporary.c_str()); return false; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), output) == text.size()
        && std::fflush(output) == 0 && fsync(fileno(output)) == 0;
    const int closeResult = std::fclose(output);
    if (!ok || closeResult != 0) {
        systemError = errno == 0 ? EIO : errno;
        std::remove(temporary.c_str());
        return false;
    }
    std::string actual;
    if (!readOne(temporary, actual, systemError, text.size()) || actual != text) {
        systemError = EIO;
        std::remove(temporary.c_str());
        return false;
    }
    if (!syncDirectory(path, systemError)) { std::remove(temporary.c_str()); return false; }
    // Never unlink a valid target on an arbitrary rename failure.
    if (std::rename(temporary.c_str(), path.c_str()) == 0) {
        if (!syncDirectory(path, systemError)) return false;
        std::remove(backup.c_str());
        return true;
    }
    const int renameError = errno;
    if (renameError != EEXIST && renameError != ENOTEMPTY) {
        systemError = renameError;
        std::remove(temporary.c_str());
        return false;
    }
    // Horizon cannot rename over an existing file. Readers use .bak while the
    // verified new generation is being published; interruption retains old data.
    if (std::remove(backup.c_str()) != 0 && errno != ENOENT) {
        systemError = errno; std::remove(temporary.c_str()); return false;
    }
    if (std::rename(path.c_str(), backup.c_str()) != 0) {
        systemError = errno; std::remove(temporary.c_str()); return false;
    }
    if (!syncDirectory(path, systemError)
        || std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (systemError == 0) systemError = errno;
        std::rename(backup.c_str(), path.c_str());
        int ignored = 0;
        syncDirectory(path, ignored);
        std::remove(temporary.c_str());
        return false;
    }
    if (!syncDirectory(path, systemError)) return false;
    std::remove(backup.c_str());
    return true;
}

bool removeTextFileRecoverable(const std::string& path, int& systemError) {
    systemError = 0;
    // A completed record must never be resurrected from its rollback generation.
    bool removed = false;
    for (const auto& name : {path + ".bak", path}) {
        if (std::remove(name.c_str()) == 0) { removed = true; continue; }
        if (errno != ENOENT) {
            systemError = errno;
            return false;
        }
    }
    return !removed || syncDirectory(path, systemError);
}
} // namespace nxsync
