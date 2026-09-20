#include "nxsync/cloud_queue.hpp"
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

std::string escapeValue(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\') result += "\\\\";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '=') result += "\\e";
        else result.push_back(ch);
    }
    return result;
}

bool unescapeValue(const std::string& value, std::string& result) {
    result.clear();
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') {
            escaped = true;
        } else if (escaped) {
            if (ch == '\\') result.push_back('\\');
            else if (ch == 'n') result.push_back('\n');
            else if (ch == 'r') result.push_back('\r');
            else if (ch == 'e') result.push_back('=');
            else return false;
            escaped = false;
        } else {
            result.push_back(ch);
        }
    }
    return !escaped;
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
        if (!current.empty() && mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

std::string operationPath(const std::string& root, const std::string& revisionId) {
    return root + (root.empty() || root.back() == '/' ? "" : "/")
        + revisionId + ".queue";
}

bool isStorageEnvironment(const std::string& value) {
    return value == "emummc" || value == "sysmmc";
}

std::string effectiveStorageEnvironment(const PendingCloudOperation& operation) {
    // Version 1 predates environment tagging. Keep those operations usable on
    // emuMMC, but never let sysMMC claim them.
    return isStorageEnvironment(operation.storageEnvironment)
        ? operation.storageEnvironment
        : "emummc";
}

std::string operationPath(
    const std::string& root,
    const PendingCloudOperation& operation) {
    if (operation.version < 2 || operation.storageEnvironment.empty()) {
        return operationPath(root, operation.revisionId);
    }
    return root + (root.empty() || root.back() == '/' ? "" : "/")
        + operation.storageEnvironment + "-" + operation.revisionId + ".queue";
}

bool writeOperation(
    const std::string& root,
    const PendingCloudOperation& operation,
    int& systemError) {
    if (!createDirectories(root, systemError)) return false;
    const std::string path = operationPath(root, operation);
    return writeTextFileAtomic(path, serializePendingCloudOperation(operation), systemError);
}

} // namespace

bool validatePendingCloudOperation(
    const PendingCloudOperation& operation,
    std::string& error) {
    if ((operation.version < 1 || operation.version > CloudOperationVersion)
        || !isHex(operation.revisionId, 64)
        || !isHex(operation.titleId, 16) || !isHex(operation.saveDataId, 16)
        || !isHex(operation.profileUid, 32)
        || !isHex(operation.archiveSha256, 64)) {
        error = "Invalid cloud queue identity";
        return false;
    }
    if (operation.version >= 2
        && !isStorageEnvironment(operation.storageEnvironment)) {
        error = "Invalid cloud queue environment";
        return false;
    }
    if (operation.archivePath.empty() || operation.remotePath.empty()
        || operation.remotePath.front() != '/') {
        error = "Invalid cloud queue paths";
        return false;
    }
    error.clear();
    return true;
}

std::string serializePendingCloudOperation(const PendingCloudOperation& operation) {
    return "version=" + std::to_string(operation.version) + "\n"
        + (operation.version >= 2
            ? "storage_environment="
                + escapeValue(operation.storageEnvironment) + "\n"
            : std::string())
        + "revision_id=" + escapeValue(operation.revisionId) + "\n"
        + "title_id=" + escapeValue(operation.titleId) + "\n"
        + "save_data_id=" + escapeValue(operation.saveDataId) + "\n"
        + "profile_uid=" + escapeValue(operation.profileUid) + "\n"
        + "archive_path=" + escapeValue(operation.archivePath) + "\n"
        + "remote_path=" + escapeValue(operation.remotePath) + "\n"
        + "archive_sha256=" + escapeValue(operation.archiveSha256) + "\n"
        + "attempt_count=" + std::to_string(operation.attemptCount) + "\n"
        + "last_error=" + escapeValue(operation.lastError) + "\n";
}

bool parsePendingCloudOperation(
    const std::string& text,
    PendingCloudOperation& operation,
    std::string& error) {
    operation = PendingCloudOperation{};
    bool versionSeen = false;
    bool environmentSeen = false;
    bool attemptsSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            std::string value;
            if (!unescapeValue(line.substr(separator + 1), value)) {
                error = "Invalid escape sequence in the cloud queue";
                return false;
            }
            std::uint64_t number = 0;
            if (key == "version" && parseUnsigned(value, number)) {
                operation.version = static_cast<unsigned>(number);
                versionSeen = true;
            }
            else if (key == "storage_environment") {
                operation.storageEnvironment = value;
                environmentSeen = true;
            }
            else if (key == "revision_id") operation.revisionId = value;
            else if (key == "title_id") operation.titleId = value;
            else if (key == "save_data_id") operation.saveDataId = value;
            else if (key == "profile_uid") operation.profileUid = value;
            else if (key == "archive_path") operation.archivePath = value;
            else if (key == "remote_path") operation.remotePath = value;
            else if (key == "archive_sha256") operation.archiveSha256 = value;
            else if (key == "attempt_count" && parseUnsigned(value, number)) {
                operation.attemptCount = static_cast<std::size_t>(number);
                attemptsSeen = true;
            }
            else if (key == "last_error") operation.lastError = value;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !attemptsSeen) {
        error = "Required cloud queue fields are missing";
        return false;
    }
    if (operation.version >= 2 && !environmentSeen) {
        error = "Cloud queue environment is missing";
        return false;
    }
    return validatePendingCloudOperation(operation, error);
}

std::vector<PendingCloudOperation> loadPendingCloudOperations(
    const std::string& queueRoot,
    const std::string& storageEnvironment) {
    std::vector<PendingCloudOperation> operations;
#ifdef __SWITCH__
    const std::string nativeRoot = queueRoot.rfind("sdmc:", 0) == 0
        ? queueRoot.substr(5)
        : queueRoot;
    if (nativeRoot.empty() || nativeRoot.front() != '/'
        || nativeRoot.size() >= FS_MAX_PATH) {
        return operations;
    }

    FsFileSystem filesystem{};
    Result result = fsOpenSdCardFileSystem(&filesystem);
    if (R_FAILED(result)) return operations;

    std::vector<std::string> names;
    FsDir directory{};
    result = fsFsOpenDirectory(
        &filesystem,
        nativeRoot.c_str(),
        FsDirOpenMode_ReadFiles,
        &directory);
    if (R_FAILED(result)) {
        fsFsClose(&filesystem);
        return operations;
    }
    while (true) {
        FsDirectoryEntry entry{};
        s64 count = 0;
        result = fsDirRead(&directory, &count, 1, &entry);
        if (R_FAILED(result) || count == 0) break;
        const std::string name = entry.name;
        if (name.size() <= 6 || (name.substr(name.size() - 6) != ".queue"
            && (name.size() <= 10 || name.substr(name.size() - 10) != ".queue.bak"))) continue;
        names.push_back(name);
    }
    fsDirClose(&directory);
    if (R_FAILED(result)) {
        fsFsClose(&filesystem);
        return operations;
    }

    for (const std::string& name : names) {
        if (name.size() > 4 && name.substr(name.size() - 4) == ".bak"
            && std::find(names.begin(), names.end(), name.substr(0, name.size() - 4)) != names.end()) continue;
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
        PendingCloudOperation operation;
        std::string error;
        if (parsePendingCloudOperation(text, operation, error)) {
            if (!storageEnvironment.empty()
                && effectiveStorageEnvironment(operation)
                    != storageEnvironment) {
                continue;
            }
            operations.push_back(std::move(operation));
        }
    }
    fsFsClose(&filesystem);
#else
    DIR* directory = opendir(queueRoot.c_str());
    if (directory == nullptr) return operations;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.size() <= 6 || (name.substr(name.size() - 6) != ".queue"
            && (name.size() <= 10 || name.substr(name.size() - 10) != ".queue.bak"))) continue;
        const std::string canonical = name.size() > 4 && name.substr(name.size() - 4) == ".bak"
            ? name.substr(0, name.size() - 4) : name;
        std::string text;
        int readError = 0;
        if (!readTextFileRecoverable(queueRoot + "/" + canonical, text, readError)) continue;
        PendingCloudOperation operation;
        std::string error;
        if (parsePendingCloudOperation(text, operation, error)) {
            if (!storageEnvironment.empty()
                && effectiveStorageEnvironment(operation) != storageEnvironment) {
                continue;
            }
            operations.push_back(std::move(operation));
        }
    }
    closedir(directory);
#endif
    std::sort(operations.begin(), operations.end(), [](const auto& left, const auto& right) {
        const std::string leftEnvironment = effectiveStorageEnvironment(left);
        const std::string rightEnvironment = effectiveStorageEnvironment(right);
        return leftEnvironment == rightEnvironment
            ? left.revisionId < right.revisionId
            : leftEnvironment < rightEnvironment;
    });
    operations.erase(std::unique(operations.begin(), operations.end(), [](const auto& a, const auto& b) {
        return a.storageEnvironment == b.storageEnvironment && a.revisionId == b.revisionId;
    }), operations.end());
    return operations;
}

bool enqueueCloudOperation(
    const std::string& queueRoot,
    const PendingCloudOperation& operation,
    int& systemError) {
    std::string error;
    if (!validatePendingCloudOperation(operation, error)) {
        systemError = EINVAL;
        return false;
    }
    const auto previous = loadPendingCloudOperations(
        queueRoot, effectiveStorageEnvironment(operation));
    const bool alreadyRecorded = std::any_of(previous.begin(), previous.end(), [&](const auto& existing) {
        return existing.revisionId == operation.revisionId
            && operationPath(queueRoot, existing) == operationPath(queueRoot, operation);
    });
    if (!alreadyRecorded && !writeOperation(queueRoot, operation, systemError)) return false;
    for (const PendingCloudOperation& existing : previous) {
        if (operationPath(queueRoot, existing) == operationPath(queueRoot, operation)) continue;
        if (existing.revisionId == operation.revisionId
            || (existing.titleId == operation.titleId
            && existing.saveDataId == operation.saveDataId
            && existing.profileUid == operation.profileUid)) {
            if (!removeTextFileRecoverable(operationPath(queueRoot, existing), systemError)) {
                return false;
            }
        }
    }
    return true;
}

bool recordCloudOperationFailure(
    const std::string& queueRoot,
    PendingCloudOperation operation,
    const std::string& message,
    int& systemError) {
    const PendingCloudOperation original = operation;
    for (const PendingCloudOperation& existing : loadPendingCloudOperations(
            queueRoot, effectiveStorageEnvironment(operation))) {
        if (existing.revisionId == operation.revisionId) {
            operation.attemptCount = existing.attemptCount;
            break;
        }
    }
    operation.version = CloudOperationVersion;
    operation.storageEnvironment = effectiveStorageEnvironment(original);
    ++operation.attemptCount;
    operation.lastError = message;
    if (!writeOperation(queueRoot, operation, systemError)) return false;
    if (original.version < 2 || original.storageEnvironment.empty()) {
        const std::string legacyPath = operationPath(
            queueRoot, original.revisionId);
        if (std::remove(legacyPath.c_str()) != 0 && errno != ENOENT) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

bool completeCloudOperation(
    const std::string& queueRoot,
    const PendingCloudOperation& operation,
    int& systemError) {
    std::string validationError;
    if (!validatePendingCloudOperation(operation, validationError)) {
        systemError = EINVAL;
        return false;
    }
    return removeTextFileRecoverable(operationPath(queueRoot, operation), systemError);
}

} // namespace nxsync
