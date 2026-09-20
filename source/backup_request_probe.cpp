#include "nxsync/backup_request_probe.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <dirent.h>
#endif

namespace nxsync {
namespace {

constexpr const char* RequestSuffix = ".request";

struct MinimalRequest {
    std::uint64_t version{0};
    std::uint64_t eventSequence{0};
    std::uint64_t requestedMonotonicNs{0};
    std::uint64_t notBeforeMonotonicNs{0};
    std::uint64_t settleDelaySeconds{0};
    char storageEnvironment[8]{};
    char titleId[17]{};
    char triggerEvent[6]{};
    bool versionSeen{false};
    bool environmentSeen{false};
    bool titleSeen{false};
    bool triggerSeen{false};
    bool sequenceSeen{false};
    bool requestedSeen{false};
    bool notBeforeSeen{false};
    bool delaySeen{false};
    bool attemptsSeen{false};
    bool lastAttemptSeen{false};
    bool lastErrorSeen{false};
};

bool equalIgnoreCase(const char left, const char right) {
    return std::tolower(static_cast<unsigned char>(left))
        == std::tolower(static_cast<unsigned char>(right));
}

bool endsWithRequestSuffix(const char* name) {
    const std::size_t nameLength = std::strlen(name);
    const std::size_t suffixLength = std::strlen(RequestSuffix);
    if (nameLength <= suffixLength) return false;
    const char* suffix = name + nameLength - suffixLength;
    for (std::size_t index = 0; index < suffixLength; ++index) {
        if (!equalIgnoreCase(suffix[index], RequestSuffix[index])) return false;
    }
    return true;
}

bool startsWith(const char* value, const char* prefix) {
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool filenameMayBelongToEnvironment(
    const char* name,
    const char* environment) {
    if (std::strcmp(environment, "sysmmc") == 0) {
        return startsWith(name, "sysmmc-");
    }
    if (std::strcmp(environment, "emummc") == 0) {
        return !startsWith(name, "sysmmc-");
    }
    return false;
}

bool parseUnsigned(const char* value, std::uint64_t& output) {
    if (value == nullptr || *value == '\0') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool copyExact(char* output, const std::size_t capacity, const char* value) {
    const std::size_t length = std::strlen(value);
    if (length + 1 > capacity) return false;
    std::memcpy(output, value, length + 1);
    return true;
}

bool isHexTitleId(const char* value) {
    if (std::strlen(value) != 16) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

void consumeLine(char* line, MinimalRequest& request) {
    const std::size_t length = std::strlen(line);
    std::size_t trimmedLength = length;
    while (trimmedLength > 0
        && (line[trimmedLength - 1] == '\n'
            || line[trimmedLength - 1] == '\r')) {
        line[--trimmedLength] = '\0';
    }
    char* separator = std::strchr(line, '=');
    if (separator == nullptr) return;
    *separator = '\0';
    const char* value = separator + 1;
    std::uint64_t number = 0;
    if (std::strcmp(line, "version") == 0 && parseUnsigned(value, number)) {
        request.version = number;
        request.versionSeen = true;
    } else if (std::strcmp(line, "storage_environment") == 0) {
        request.environmentSeen = copyExact(
            request.storageEnvironment,
            sizeof(request.storageEnvironment),
            value);
    } else if (std::strcmp(line, "title_id") == 0) {
        request.titleSeen = copyExact(
            request.titleId, sizeof(request.titleId), value);
    } else if (std::strcmp(line, "trigger_event") == 0) {
        request.triggerSeen = copyExact(
            request.triggerEvent, sizeof(request.triggerEvent), value);
    } else if (std::strcmp(line, "event_sequence") == 0
        && parseUnsigned(value, number)) {
        request.eventSequence = number;
        request.sequenceSeen = true;
    } else if (std::strcmp(line, "requested_monotonic_ns") == 0
        && parseUnsigned(value, number)) {
        request.requestedMonotonicNs = number;
        request.requestedSeen = true;
    } else if (std::strcmp(line, "not_before_monotonic_ns") == 0
        && parseUnsigned(value, number)) {
        request.notBeforeMonotonicNs = number;
        request.notBeforeSeen = true;
    } else if (std::strcmp(line, "settle_delay_seconds") == 0
        && parseUnsigned(value, number)) {
        request.settleDelaySeconds = number;
        request.delaySeen = true;
    } else if (std::strcmp(line, "attempt_count") == 0
        && parseUnsigned(value, number)) {
        request.attemptsSeen = true;
    } else if (std::strcmp(line, "last_attempt_monotonic_ns") == 0
        && parseUnsigned(value, number)) {
        request.lastAttemptSeen = true;
    } else if (std::strcmp(line, "last_error") == 0) {
        request.lastErrorSeen = true;
    }
}

bool isEnvironment(const char* value) {
    return std::strcmp(value, "emummc") == 0
        || std::strcmp(value, "sysmmc") == 0;
}

const char* effectiveEnvironment(const MinimalRequest& request) {
    return request.version >= 3 ? request.storageEnvironment : "emummc";
}

bool isValid(const MinimalRequest& request) {
    return request.versionSeen
        && request.version >= 1
        && request.version <= 3
        && request.titleSeen
        && isHexTitleId(request.titleId)
        && request.triggerSeen
        && (std::strcmp(request.triggerEvent, "exit") == 0
            || std::strcmp(request.triggerEvent, "crash") == 0)
        && request.sequenceSeen
        && request.eventSequence != 0
        && request.requestedSeen
        && request.notBeforeSeen
        && request.delaySeen
        && request.settleDelaySeconds <= 300
        && request.notBeforeMonotonicNs >= request.requestedMonotonicNs
        && (request.version < 2
            || (request.attemptsSeen
                && request.lastAttemptSeen
                && request.lastErrorSeen))
        && (request.version < 3
            || (request.environmentSeen
                && isEnvironment(request.storageEnvironment)));
}

bool isReady(
    const MinimalRequest& request,
    const std::uint64_t currentMonotonicNs) {
    return currentMonotonicNs < request.requestedMonotonicNs
        || currentMonotonicNs >= request.notBeforeMonotonicNs;
}

#ifdef __SWITCH__

constexpr Result FsPathNotFound = 0x202;
constexpr std::uint64_t MaximumRequestSize = 64 * 1024;

bool setNativeFailure(
    BackupRequestProbeSummary& summary,
    const BackupRequestProbeStage stage,
    const Result result) {
    summary.nativeResult = static_cast<std::uint32_t>(R_VALUE(result));
    summary.failureStage = stage;
    return false;
}

bool readNativeRequest(
    FsFileSystem& filesystem,
    const char* path,
    MinimalRequest& request,
    BackupRequestProbeSummary& summary) {
    FsFile file{};
    Result result = fsFsOpenFile(
        &filesystem, path, FsOpenMode_Read, &file);
    if (R_FAILED(result)) {
        return setNativeFailure(
            summary, BackupRequestProbeStage::OpenRequest, result);
    }

    s64 signedSize = 0;
    result = fsFileGetSize(&file, &signedSize);
    if (R_FAILED(result)) {
        fsFileClose(&file);
        return setNativeFailure(
            summary, BackupRequestProbeStage::GetRequestSize, result);
    }
    if (signedSize < 0
        || static_cast<std::uint64_t>(signedSize) > MaximumRequestSize) {
        fsFileClose(&file);
        summary.systemError = EFBIG;
        summary.failureStage = BackupRequestProbeStage::GetRequestSize;
        return false;
    }

    char chunk[256]{};
    char line[384]{};
    std::size_t lineLength = 0;
    bool lineOverflow = false;
    std::uint64_t offset = 0;
    const std::uint64_t fileSize = static_cast<std::uint64_t>(signedSize);
    while (offset < fileSize) {
        const std::uint64_t remaining = fileSize - offset;
        const std::uint64_t requested = remaining < sizeof(chunk)
            ? remaining
            : sizeof(chunk);
        u64 bytesRead = 0;
        result = fsFileRead(
            &file,
            offset,
            chunk,
            requested,
            FsReadOption_None,
            &bytesRead);
        if (R_FAILED(result)) {
            fsFileClose(&file);
            return setNativeFailure(
                summary, BackupRequestProbeStage::ReadRequest, result);
        }
        if (bytesRead != requested) {
            fsFileClose(&file);
            summary.systemError = EIO;
            summary.failureStage = BackupRequestProbeStage::ReadRequest;
            return false;
        }
        for (std::uint64_t index = 0; index < bytesRead; ++index) {
            const char value = chunk[index];
            if (value == '\n') {
                if (!lineOverflow) {
                    line[lineLength] = '\0';
                    consumeLine(line, request);
                }
                lineLength = 0;
                lineOverflow = false;
            } else if (!lineOverflow) {
                if (lineLength + 1 < sizeof(line)) {
                    line[lineLength++] = value;
                } else {
                    lineOverflow = true;
                }
            }
        }
        offset += bytesRead;
    }
    fsFileClose(&file);
    if (!lineOverflow && lineLength > 0) {
        line[lineLength] = '\0';
        consumeLine(line, request);
    }
    return !lineOverflow;
}

#endif

} // namespace

const char* backupRequestProbeStageName(const BackupRequestProbeStage stage) {
    switch (stage) {
        case BackupRequestProbeStage::OpenFileSystem: return "fs-open-sd";
        case BackupRequestProbeStage::OpenDirectory: return "fs-open-directory";
        case BackupRequestProbeStage::ReadDirectory: return "fs-read-directory";
        case BackupRequestProbeStage::CloseDirectory: return "fs-close-directory";
        case BackupRequestProbeStage::BuildPath: return "build-path";
        case BackupRequestProbeStage::OpenRequest: return "fs-open-file";
        case BackupRequestProbeStage::GetRequestSize: return "file-size";
        case BackupRequestProbeStage::ReadRequest: return "fs-read-file";
        case BackupRequestProbeStage::CloseRequest: return "fs-close-file";
        case BackupRequestProbeStage::None: return "none";
    }
    return "unknown";
}

BackupRequestProbeSummary probePendingBackupRequests(
    const char* queueRoot,
    const char* storageEnvironment,
    const std::uint64_t currentMonotonicNs,
    const std::uint64_t maximumWaitNs) {
    BackupRequestProbeSummary summary;
    summary.nextWaitNs = maximumWaitNs;
#ifdef __SWITCH__
    if (queueRoot == nullptr || storageEnvironment == nullptr) {
        summary.systemError = EINVAL;
        summary.failureStage = BackupRequestProbeStage::BuildPath;
        return summary;
    }
    const char* nativeRoot = std::strncmp(queueRoot, "sdmc:", 5) == 0
        ? queueRoot + 5
        : queueRoot;
    if (nativeRoot[0] != '/' || std::strlen(nativeRoot) >= FS_MAX_PATH) {
        summary.systemError = ENAMETOOLONG;
        summary.failureStage = BackupRequestProbeStage::BuildPath;
        return summary;
    }

    FsFileSystem filesystem{};
    Result result = fsOpenSdCardFileSystem(&filesystem);
    if (R_FAILED(result)) {
        setNativeFailure(
            summary, BackupRequestProbeStage::OpenFileSystem, result);
        return summary;
    }

    char previousName[FS_MAX_PATH]{};
    while (true) {
        char selectedName[FS_MAX_PATH]{};
        FsDir directory{};
        result = fsFsOpenDirectory(
            &filesystem,
            nativeRoot,
            FsDirOpenMode_ReadFiles,
            &directory);
        if (R_FAILED(result)) {
            if (R_VALUE(result) != FsPathNotFound) {
                setNativeFailure(
                    summary, BackupRequestProbeStage::OpenDirectory, result);
            }
            fsFsClose(&filesystem);
            return summary;
        }
        while (true) {
            FsDirectoryEntry entry{};
            s64 count = 0;
            result = fsDirRead(&directory, &count, 1, &entry);
            if (R_FAILED(result) || count == 0) break;
            const char* name = entry.name;
            if (!endsWithRequestSuffix(name)
                || !filenameMayBelongToEnvironment(name, storageEnvironment)
                || (previousName[0] != '\0'
                    && std::strcmp(name, previousName) <= 0)
                || (selectedName[0] != '\0'
                    && std::strcmp(name, selectedName) >= 0)) {
                continue;
            }
            if (!copyExact(selectedName, sizeof(selectedName), name)) {
                summary.systemError = ENAMETOOLONG;
                summary.failureStage = BackupRequestProbeStage::BuildPath;
                break;
            }
        }
        fsDirClose(&directory);
        if (R_FAILED(result)) {
            setNativeFailure(
                summary, BackupRequestProbeStage::ReadDirectory, result);
            fsFsClose(&filesystem);
            return summary;
        }
        if (summary.systemError != 0) {
            fsFsClose(&filesystem);
            return summary;
        }
        if (selectedName[0] == '\0') break;
        std::memcpy(previousName, selectedName, sizeof(previousName));

        char path[FS_MAX_PATH]{};
        const int pathLength = std::snprintf(
            path, sizeof(path), "%s/%s", nativeRoot, selectedName);
        if (pathLength < 0
            || static_cast<std::size_t>(pathLength) >= sizeof(path)) {
            ++summary.invalidCount;
            summary.systemError = ENAMETOOLONG;
            summary.failureStage = BackupRequestProbeStage::BuildPath;
            fsFsClose(&filesystem);
            return summary;
        }

        MinimalRequest request;
        if (!readNativeRequest(filesystem, path, request, summary)) {
            ++summary.invalidCount;
            fsFsClose(&filesystem);
            return summary;
        }
        if (!isValid(request)) {
            ++summary.invalidCount;
            continue;
        }
        if (std::strcmp(
                effectiveEnvironment(request), storageEnvironment) != 0) {
            continue;
        }

        ++summary.validCount;
        if (request.eventSequence >= summary.latestEventSequence) {
            summary.latestEventSequence = request.eventSequence;
            std::memcpy(
                summary.latestTitleId.data(),
                request.titleId,
                sizeof(request.titleId));
        }
        if (isReady(request, currentMonotonicNs)) {
            summary.hasReadyRequest = true;
            summary.nextWaitNs = 0;
        } else if (!summary.hasReadyRequest) {
            const std::uint64_t requestWait =
                request.notBeforeMonotonicNs - currentMonotonicNs;
            if (requestWait < summary.nextWaitNs) {
                summary.nextWaitNs = requestWait;
            }
        }
    }
    fsFsClose(&filesystem);
    return summary;
#else
    char previousName[256]{};
    while (true) {
        // The resident process exposes a single fs session. Never keep a
        // directory handle open while opening a request file: fsdev would need
        // a second session and return ENOSR. Select one name, close the
        // directory, then inspect that file before enumerating again.
        char selectedName[256]{};
        DIR* directory = opendir(queueRoot);
        if (directory == nullptr) {
            if (errno != ENOENT) {
                summary.systemError = errno;
                summary.failureStage = BackupRequestProbeStage::OpenDirectory;
            }
            return summary;
        }
        errno = 0;
        while (dirent* entry = readdir(directory)) {
            const char* name = entry->d_name;
            if (!endsWithRequestSuffix(name)
                || !filenameMayBelongToEnvironment(name, storageEnvironment)
                || (previousName[0] != '\0'
                    && std::strcmp(name, previousName) <= 0)
                || (selectedName[0] != '\0'
                    && std::strcmp(name, selectedName) >= 0)) {
                continue;
            }
            if (!copyExact(selectedName, sizeof(selectedName), name)) {
                if (summary.systemError == 0) {
                    summary.systemError = ENAMETOOLONG;
                    summary.failureStage = BackupRequestProbeStage::BuildPath;
                }
            }
        }
        const int readDirectoryError = errno;
        if (closedir(directory) != 0 && summary.systemError == 0) {
            summary.systemError = errno;
            summary.failureStage = BackupRequestProbeStage::CloseDirectory;
        }
        if (readDirectoryError != 0
            && readDirectoryError != ENOENT
            && summary.systemError == 0) {
            summary.systemError = readDirectoryError;
            summary.failureStage = BackupRequestProbeStage::ReadDirectory;
        }
        if (summary.systemError != 0) return summary;
        if (selectedName[0] == '\0') break;
        std::memcpy(previousName, selectedName, sizeof(previousName));

        char path[512]{};
        const int pathLength = std::snprintf(
            path, sizeof(path), "%s/%s", queueRoot, selectedName);
        if (pathLength < 0
            || static_cast<std::size_t>(pathLength) >= sizeof(path)) {
            ++summary.invalidCount;
            if (summary.systemError == 0) {
                summary.systemError = ENAMETOOLONG;
                summary.failureStage = BackupRequestProbeStage::BuildPath;
            }
            continue;
        }
        FILE* input = std::fopen(path, "rb");
        if (input == nullptr) {
            ++summary.invalidCount;
            if (summary.systemError == 0) {
                summary.systemError = errno;
                summary.failureStage = BackupRequestProbeStage::OpenRequest;
            }
            continue;
        }

        MinimalRequest request;
        char line[384]{};
        while (std::fgets(line, sizeof(line), input) != nullptr) {
            consumeLine(line, request);
        }
        const bool readFailed = std::ferror(input) != 0;
        const int closeResult = std::fclose(input);

        if (readFailed) {
            ++summary.invalidCount;
            if (summary.systemError == 0) {
                summary.systemError = EIO;
                summary.failureStage = BackupRequestProbeStage::ReadRequest;
            }
            continue;
        }
        if (closeResult != 0) {
            ++summary.invalidCount;
            if (summary.systemError == 0) {
                summary.systemError = errno;
                summary.failureStage = BackupRequestProbeStage::CloseRequest;
            }
            continue;
        }

        if (!isValid(request)) {
            ++summary.invalidCount;
            continue;
        }
        if (std::strcmp(
                effectiveEnvironment(request), storageEnvironment) != 0) {
            continue;
        }

        ++summary.validCount;
        if (request.eventSequence >= summary.latestEventSequence) {
            summary.latestEventSequence = request.eventSequence;
            std::memcpy(
                summary.latestTitleId.data(), request.titleId, sizeof(request.titleId));
        }
        if (isReady(request, currentMonotonicNs)) {
            summary.hasReadyRequest = true;
            summary.nextWaitNs = 0;
        } else if (!summary.hasReadyRequest) {
            const std::uint64_t requestWait =
                request.notBeforeMonotonicNs - currentMonotonicNs;
            if (requestWait < summary.nextWaitNs) {
                summary.nextWaitNs = requestWait;
            }
        }
    }
    return summary;
#endif
}

} // namespace nxsync
