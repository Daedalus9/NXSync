#include "nxsync/backup_request_queue.hpp"
#include "nxsync/atomic_file.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <utility>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <dirent.h>
#endif

namespace nxsync {
namespace {

bool isHex(const std::string& value, const std::size_t length) {
    return value.size() == length
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

bool parseUnsigned(const std::string& value, std::uint64_t& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool createDirectories(const std::string& path, int& systemError) {
    const std::size_t deviceEnd = path.find(":/");
    const std::size_t start = deviceEnd == std::string::npos ? 1 : deviceEnd + 2;
    for (std::size_t index = start; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/') continue;
        const std::string current = path.substr(0, index);
        if (!current.empty() && mkdir(current.c_str(), 0777) != 0
            && errno != EEXIST) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

std::string requestPath(const std::string& root, const std::string& titleId) {
    return root + (root.empty() || root.back() == '/' ? "" : "/")
        + titleId + ".request";
}

bool isStorageEnvironment(const std::string& value) {
    return value == "emummc" || value == "sysmmc";
}

std::string effectiveStorageEnvironment(const PendingBackupRequest& request) {
    // Requests written before v3 have no origin. NXSync historically ran its
    // automations on emuMMC, so treating legacy data as emuMMC-only is the
    // conservative migration: sysMMC can never claim it accidentally.
    return isStorageEnvironment(request.storageEnvironment)
        ? request.storageEnvironment
        : "emummc";
}

std::string requestPath(
    const std::string& root,
    const PendingBackupRequest& request) {
    if (request.version < 3 || request.storageEnvironment.empty()) {
        return requestPath(root, request.titleId);
    }
    return root + (root.empty() || root.back() == '/' ? "" : "/")
        + request.storageEnvironment + "-" + request.titleId + ".request";
}

} // namespace

bool validatePendingBackupRequest(
    const PendingBackupRequest& request,
    std::string& error) {
    if ((request.version < 1 || request.version > BackupRequestVersion)
        || !isHex(request.titleId, 16)
        || (request.triggerEvent != "exit" && request.triggerEvent != "crash")
        || request.eventSequence == 0) {
        error = "Invalid backup request identity";
        return false;
    }
    if (request.version >= 3
        && !isStorageEnvironment(request.storageEnvironment)) {
        error = "Invalid backup request environment";
        return false;
    }
    if (request.settleDelaySeconds > 300
        || request.notBeforeMonotonicNs < request.requestedMonotonicNs
        || request.lastError.find_first_of("\r\n") != std::string::npos) {
        error = "Invalid backup request delay";
        return false;
    }
    error.clear();
    return true;
}

std::string serializePendingBackupRequest(const PendingBackupRequest& request) {
    return "version=" + std::to_string(request.version) + "\n"
        + (request.version >= 3
            ? "storage_environment=" + request.storageEnvironment + "\n"
            : std::string())
        + "title_id=" + request.titleId + "\n"
        + "trigger_event=" + request.triggerEvent + "\n"
        + "event_sequence=" + std::to_string(request.eventSequence) + "\n"
        + "requested_monotonic_ns="
            + std::to_string(request.requestedMonotonicNs) + "\n"
        + "not_before_monotonic_ns="
            + std::to_string(request.notBeforeMonotonicNs) + "\n"
        + "settle_delay_seconds="
            + std::to_string(request.settleDelaySeconds) + "\n"
        + "attempt_count=" + std::to_string(request.attemptCount) + "\n"
        + "last_attempt_monotonic_ns="
            + std::to_string(request.lastAttemptMonotonicNs) + "\n"
        + "last_error=" + request.lastError + "\n";
}

bool parsePendingBackupRequest(
    const std::string& text,
    PendingBackupRequest& request,
    std::string& error) {
    request = PendingBackupRequest{};
    bool versionSeen = false;
    bool environmentSeen = false;
    bool titleSeen = false;
    bool triggerSeen = false;
    bool sequenceSeen = false;
    bool requestedSeen = false;
    bool notBeforeSeen = false;
    bool delaySeen = false;
    bool attemptsSeen = false;
    bool lastAttemptSeen = false;
    bool lastErrorSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            const std::string value = line.substr(separator + 1);
            std::uint64_t number = 0;
            if (key == "version" && parseUnsigned(value, number)) {
                request.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "storage_environment") {
                request.storageEnvironment = value;
                environmentSeen = true;
            } else if (key == "title_id") {
                request.titleId = value;
                titleSeen = true;
            } else if (key == "trigger_event") {
                request.triggerEvent = value;
                triggerSeen = true;
            } else if (key == "event_sequence" && parseUnsigned(value, number)) {
                request.eventSequence = number;
                sequenceSeen = true;
            } else if (key == "requested_monotonic_ns"
                && parseUnsigned(value, number)) {
                request.requestedMonotonicNs = number;
                requestedSeen = true;
            } else if (key == "not_before_monotonic_ns"
                && parseUnsigned(value, number)) {
                request.notBeforeMonotonicNs = number;
                notBeforeSeen = true;
            } else if (key == "settle_delay_seconds"
                && parseUnsigned(value, number) && number <= UINT32_MAX) {
                request.settleDelaySeconds = static_cast<std::uint32_t>(number);
                delaySeen = true;
            } else if (key == "attempt_count" && parseUnsigned(value, number)) {
                request.attemptCount = static_cast<std::size_t>(number);
                attemptsSeen = true;
            } else if (key == "last_attempt_monotonic_ns"
                && parseUnsigned(value, number)) {
                request.lastAttemptMonotonicNs = number;
                lastAttemptSeen = true;
            } else if (key == "last_error") {
                request.lastError = value;
                lastErrorSeen = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !titleSeen || !triggerSeen || !sequenceSeen
        || !requestedSeen || !notBeforeSeen || !delaySeen) {
        error = "Required backup request fields are missing";
        return false;
    }
    if (request.version >= 2
        && (!attemptsSeen || !lastAttemptSeen || !lastErrorSeen)) {
        error = "Backup request retry fields are missing";
        return false;
    }
    if (request.version >= 3 && !environmentSeen) {
        error = "Backup request environment is missing";
        return false;
    }
    return validatePendingBackupRequest(request, error);
}

std::vector<PendingBackupRequest> loadPendingBackupRequests(
    const std::string& queueRoot,
    const std::string& storageEnvironment) {
    std::vector<PendingBackupRequest> requests;
    std::vector<std::string> names;
#ifdef __SWITCH__
    const std::string nativeRoot = queueRoot.rfind("sdmc:", 0) == 0
        ? queueRoot.substr(5)
        : queueRoot;
    if (nativeRoot.empty() || nativeRoot.front() != '/'
        || nativeRoot.size() >= FS_MAX_PATH) {
        return requests;
    }

    FsFileSystem filesystem{};
    Result result = fsOpenSdCardFileSystem(&filesystem);
    if (R_FAILED(result)) return requests;

    FsDir directory{};
    result = fsFsOpenDirectory(
        &filesystem,
        nativeRoot.c_str(),
        FsDirOpenMode_ReadFiles,
        &directory);
    if (R_FAILED(result)) {
        fsFsClose(&filesystem);
        return requests;
    }
    while (true) {
        FsDirectoryEntry entry{};
        s64 count = 0;
        result = fsDirRead(&directory, &count, 1, &entry);
        if (R_FAILED(result) || count == 0) break;
        const std::string name = entry.name;
        if (!(name.size() > 8 && name.substr(name.size() - 8) == ".request")
            && !(name.size() > 12 && name.substr(name.size() - 12) == ".request.bak")) continue;
        names.push_back(name);
    }
    fsDirClose(&directory);
    if (R_FAILED(result)) {
        fsFsClose(&filesystem);
        return requests;
    }

    for (const std::string& name : names) {
        const bool backup = name.size() > 12 && name.substr(name.size()-12) == ".request.bak";
        const std::string canonical = backup ? name.substr(0, name.size()-4) : name;
        if (backup && std::find(names.begin(), names.end(), canonical) != names.end()) continue;
        const std::string path = nativeRoot + "/" + name;
        if (path.size() >= FS_MAX_PATH) continue;
        FsFile file{};
        result = fsFsOpenFile(
            &filesystem, path.c_str(), FsOpenMode_Read, &file);
        if (R_FAILED(result)) continue;
        s64 fileSize = 0;
        result = fsFileGetSize(&file, &fileSize);
        if (R_FAILED(result) || fileSize < 0 || fileSize > 64 * 1024) {
            fsFileClose(&file);
            continue;
        }
        std::string text(static_cast<std::size_t>(fileSize), '\0');
        u64 bytesRead = 0;
        if (fileSize > 0) {
            result = fsFileRead(
                &file,
                0,
                text.data(),
                static_cast<u64>(fileSize),
                FsReadOption_None,
                &bytesRead);
        }
        fsFileClose(&file);
        if (R_FAILED(result)
            || bytesRead != static_cast<u64>(fileSize)) continue;
        PendingBackupRequest request;
        std::string error;
        if (parsePendingBackupRequest(text, request, error)) {
            if (!storageEnvironment.empty()
                && effectiveStorageEnvironment(request) != storageEnvironment) {
                continue;
            }
            requests.push_back(std::move(request));
        }
    }
    fsFsClose(&filesystem);
#else
    DIR* directory = opendir(queueRoot.c_str());
    if (directory == nullptr) return requests;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (!(name.size() > 8 && name.substr(name.size() - 8) == ".request")
            && !(name.size() > 12 && name.substr(name.size() - 12) == ".request.bak")) continue;
        names.push_back(name);
    }
    // cloud-worker also exposes one fs session. Release the directory handle
    // before opening any request file, otherwise fsdev returns ENOSR.
    closedir(directory);
    for (const std::string& name : names) {
        const bool backup = name.size() > 12 && name.substr(name.size()-12) == ".request.bak";
        const std::string canonical = backup ? name.substr(0, name.size()-4) : name;
        if (backup && std::find(names.begin(), names.end(), canonical) != names.end()) continue;
        std::string text;
        int readError = 0;
        if (!readTextFileRecoverable(queueRoot + "/" + canonical, text, readError)) continue;
        PendingBackupRequest request;
        std::string error;
        if (parsePendingBackupRequest(text, request, error)) {
            if (!storageEnvironment.empty()
                && effectiveStorageEnvironment(request) != storageEnvironment) {
                continue;
            }
            requests.push_back(std::move(request));
        }
    }
#endif
    std::sort(requests.begin(), requests.end(), [](const auto& left, const auto& right) {
        const std::string leftEnvironment = effectiveStorageEnvironment(left);
        const std::string rightEnvironment = effectiveStorageEnvironment(right);
        return leftEnvironment == rightEnvironment
            ? left.titleId < right.titleId
            : leftEnvironment < rightEnvironment;
    });
    return requests;
}

bool enqueueBackupRequest(
    const std::string& queueRoot,
    const PendingBackupRequest& request,
    int& systemError) {
    std::string error;
    if (!validatePendingBackupRequest(request, error)) {
        systemError = EINVAL;
        return false;
    }
    if (!createDirectories(queueRoot, systemError)) return false;
    const std::string path = requestPath(queueRoot, request);
    return writeTextFileAtomic(path, serializePendingBackupRequest(request), systemError);
}

bool recordBackupRequestFailure(
    const std::string& queueRoot,
    PendingBackupRequest request,
    const std::string& message,
    const std::uint64_t currentMonotonicNs,
    const std::uint32_t retryDelaySeconds,
    int& systemError) {
    const PendingBackupRequest original = request;
    request.version = BackupRequestVersion;
    request.storageEnvironment = effectiveStorageEnvironment(original);
    ++request.attemptCount;
    request.lastAttemptMonotonicNs = currentMonotonicNs;
    request.lastError = message;
    request.requestedMonotonicNs = currentMonotonicNs;
    request.notBeforeMonotonicNs = currentMonotonicNs
        + static_cast<std::uint64_t>(retryDelaySeconds) * 1'000'000'000ULL;
    if (!enqueueBackupRequest(queueRoot, request, systemError)) return false;
    if (original.version < 3 || original.storageEnvironment.empty()) {
        const std::string legacyPath = requestPath(queueRoot, original.titleId);
        if (std::remove(legacyPath.c_str()) != 0 && errno != ENOENT) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

bool completeBackupRequest(
    const std::string& queueRoot,
    const PendingBackupRequest& request,
    int& systemError) {
    std::string validationError;
    if (!validatePendingBackupRequest(request, validationError)) {
        systemError = EINVAL;
        return false;
    }
    return removeTextFileRecoverable(requestPath(queueRoot, request), systemError);
}

bool isBackupRequestReady(
    const PendingBackupRequest& request,
    const std::uint64_t currentMonotonicNs) {
    // A lower counter indicates a reboot; a durable request from the previous
    // boot has already outlived its safety delay and may be processed.
    return currentMonotonicNs < request.requestedMonotonicNs
        || currentMonotonicNs >= request.notBeforeMonotonicNs;
}

std::uint64_t nextBackupRequestWaitDurationNs(
    const std::vector<PendingBackupRequest>& requests,
    const std::uint64_t currentMonotonicNs,
    const std::uint64_t maximumWaitNs) {
    std::uint64_t waitNs = maximumWaitNs;
    for (const PendingBackupRequest& request : requests) {
        if (isBackupRequestReady(request, currentMonotonicNs)) return 0;
        waitNs = std::min(
            waitNs,
            request.notBeforeMonotonicNs - currentMonotonicNs);
    }
    return waitNs;
}

} // namespace nxsync
