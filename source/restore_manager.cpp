#include "nxsync/restore_manager.hpp"

#include "nxsync/backup_state.hpp"

#include "nxsync/backup_format.hpp"
#include "nxsync/game_version.hpp"
#include "nxsync/revision_parents.hpp"

#include <minizip/unzip.h>
#include <switch.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nxsync {
namespace {

constexpr const char* RestoreMountName = "nxsyncrestore";
constexpr const char* RestoreMountRoot = "nxsyncrestore:/";
constexpr std::size_t BufferSize = 128 * 1024;
constexpr std::size_t MaximumFiles = 100000;
constexpr std::uint64_t MaximumArchiveBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

std::size_t jsonValuePosition(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    std::size_t lineStart = 0;
    while (lineStart < json.size()) {
        std::size_t position = lineStart;
        while (position < json.size()
            && (json[position] == ' ' || json[position] == '\t' || json[position] == '\r')) {
            ++position;
        }
        if (json.compare(position, marker.size(), marker) == 0) {
            position += marker.size();
            while (position < json.size()
                && std::isspace(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
            if (position >= json.size() || json[position] != ':') {
                return std::string::npos;
            }
            ++position;
            while (position < json.size()
                && std::isspace(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
            return position;
        }
        const std::size_t nextLine = json.find('\n', lineStart);
        if (nextLine == std::string::npos) {
            break;
        }
        lineStart = nextLine + 1;
    }
    return std::string::npos;
}

class MountedRestoreSave {
public:
    explicit MountedRestoreSave(const bool mounted) : mounted_(mounted) {}
    ~MountedRestoreSave() {
        unmount();
    }
    MountedRestoreSave(const MountedRestoreSave&) = delete;
    MountedRestoreSave& operator=(const MountedRestoreSave&) = delete;

    void unmount() {
        if (mounted_) {
            fsdevUnmountDevice(RestoreMountName);
            mounted_ = false;
        }
    }

private:
    bool mounted_{false};
};

std::string jsonString(const std::string& json, const std::string& key) {
    std::size_t position = jsonValuePosition(json, key);
    if (position >= json.size() || json[position] != '"') {
        return {};
    }
    ++position;
    std::string value;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaped) {
            switch (ch) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                default: value.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return value;
        } else {
            value.push_back(ch);
        }
    }
    return {};
}

bool jsonStringArray(
    const std::string& json,
    const std::string& key,
    std::vector<std::string>& values) {
    values.clear();
    std::size_t position = jsonValuePosition(json, key);
    if (position == std::string::npos) return true;
    if (position >= json.size() || json[position] != '[') return false;
    ++position;
    while (position < json.size()) {
        while (position < json.size()
            && std::isspace(static_cast<unsigned char>(json[position]))) {
            ++position;
        }
        if (position < json.size() && json[position] == ']') return true;
        if (position >= json.size() || json[position] != '"') return false;
        ++position;
        std::string value;
        bool escaped = false;
        bool closed = false;
        for (; position < json.size(); ++position) {
            const char ch = json[position];
            if (escaped) {
                if (ch == 'n') value.push_back('\n');
                else if (ch == 'r') value.push_back('\r');
                else if (ch == 't') value.push_back('\t');
                else if (ch == '\\' || ch == '"') value.push_back(ch);
                else return false;
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                ++position;
                closed = true;
                break;
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                return false;
            } else {
                value.push_back(ch);
            }
        }
        if (!closed) return false;
        values.push_back(value);
        while (position < json.size()
            && std::isspace(static_cast<unsigned char>(json[position]))) {
            ++position;
        }
        if (position < json.size() && json[position] == ',') {
            ++position;
            continue;
        }
        if (position < json.size() && json[position] == ']') return true;
        return false;
    }
    return false;
}

bool jsonUnsigned(
    const std::string& json,
    const std::string& key,
    std::uint64_t& output) {
    const std::size_t position = jsonValuePosition(json, key);
    if (position == std::string::npos) {
        return false;
    }
    if (position >= json.size()
        || json[position] < '0' || json[position] > '9') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(json.c_str() + position, &end, 10);
    if (errno != 0 || end == json.c_str() + position) {
        return false;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end != ',' && *end != '}') {
        return false;
    }
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parseHex64(const std::string& value, std::uint64_t& output) {
    if (value.empty() || value.size() > 16) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 16);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return false;
    }
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool isHexString(const std::string& value, const std::size_t expectedLength) {
    return value.size() == expectedLength
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

struct PayloadFileDigest {
    std::string path;
    std::uint64_t size{0};
    std::array<unsigned char, SHA256_HASH_SIZE> digest{};
};

void updateBigEndian32(Sha256Context& context, const std::uint32_t value) {
    const unsigned char encoded[] = {
        static_cast<unsigned char>((value >> 24U) & 0xFFU),
        static_cast<unsigned char>((value >> 16U) & 0xFFU),
        static_cast<unsigned char>((value >> 8U) & 0xFFU),
        static_cast<unsigned char>(value & 0xFFU),
    };
    sha256ContextUpdate(&context, encoded, sizeof(encoded));
}

void updateBigEndian64(Sha256Context& context, const std::uint64_t value) {
    unsigned char encoded[8]{};
    for (std::size_t index = 0; index < 8; ++index) {
        encoded[7 - index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
    }
    sha256ContextUpdate(&context, encoded, sizeof(encoded));
}

std::string hashToHex(const std::array<unsigned char, SHA256_HASH_SIZE>& hash) {
    static constexpr char HexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash.size() * 2);
    for (const unsigned char byte : hash) {
        result.push_back(HexDigits[byte >> 4U]);
        result.push_back(HexDigits[byte & 0x0FU]);
    }
    return result;
}

std::string calculatePayloadSha256(std::vector<PayloadFileDigest> files) {
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    Sha256Context context{};
    sha256ContextCreate(&context);
    for (const PayloadFileDigest& file : files) {
        const unsigned char tag = 'F';
        sha256ContextUpdate(&context, &tag, 1);
        updateBigEndian32(context, static_cast<std::uint32_t>(file.path.size()));
        sha256ContextUpdate(&context, file.path.data(), file.path.size());
        updateBigEndian64(context, file.size);
        sha256ContextUpdate(&context, file.digest.data(), file.digest.size());
    }
    std::array<unsigned char, SHA256_HASH_SIZE> hash{};
    sha256ContextGetHash(&context, hash.data());
    return hashToHex(hash);
}

std::int64_t alignSaveSize(const std::int64_t value) {
    constexpr std::int64_t Alignment = 0x4000;
    if (value <= 0 || value > std::numeric_limits<std::int64_t>::max() - Alignment) {
        return value;
    }
    return (value + Alignment - 1) & ~(Alignment - 1);
}

bool safeSaveEntryPath(const std::string& name, std::string& relativePath) {
    relativePath.clear();
    if (name == "nxsync-metadata.json") {
        return true;
    }
    if (name == "save/") {
        return true;
    }
    if (name.compare(0, 5, "save/") != 0 || name.size() <= 5
        || name.front() == '/' || name.find('\\') != std::string::npos
        || name.find('\0') != std::string::npos) {
        return false;
    }
    relativePath = name.substr(5);
    std::size_t start = 0;
    while (start <= relativePath.size()) {
        const std::size_t slash = relativePath.find('/', start);
        const std::string segment = relativePath.substr(
            start,
            slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty() && slash != std::string::npos && slash + 1 != relativePath.size()) {
            return false;
        }
        if (segment == "." || segment == ".." || segment.find(':') != std::string::npos) {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return !relativePath.empty();
}

bool readCurrentZipEntry(unzFile archive, std::string& output, const std::size_t maximum) {
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK
        || info.uncompressed_size > maximum
        || unzOpenCurrentFile(archive) != UNZ_OK) {
        return false;
    }
    output.clear();
    output.reserve(static_cast<std::size_t>(info.uncompressed_size));
    std::array<char, 4096> buffer{};
    while (true) {
        const int read = unzReadCurrentFile(archive, buffer.data(), buffer.size());
        if (read < 0) {
            unzCloseCurrentFile(archive);
            return false;
        }
        if (read == 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(read));
    }
    return unzCloseCurrentFile(archive) == UNZ_OK;
}

bool parseManifest(
    const std::string& json,
    RestoreManifest& manifest,
    std::string& error) {
    std::uint64_t number = 0;
    if (!jsonUnsigned(json, "format_version", number)) {
        error = "Legacy backup format is not supported";
        return false;
    }
    manifest.formatVersion = static_cast<unsigned>(number);
    manifest.revisionId = jsonString(json, "revision_id");
    manifest.parentRevisionId = jsonString(json, "parent_revision_id");
    if (!jsonStringArray(
            json, "parent_revision_ids", manifest.parentRevisionIds)) {
        error = "Invalid parent list in the manifest";
        return false;
    }
    manifest.parentRevisionIds = normalizedRevisionParents(
        manifest.parentRevisionId, manifest.parentRevisionIds);
    manifest.parentRevisionId = manifest.parentRevisionIds.empty()
        ? std::string()
        : manifest.parentRevisionIds.front();
    manifest.payloadSha256 = jsonString(json, "payload_sha256");
    const BackupFormatIdentity formatIdentity{
        jsonString(json, "schema"),
        manifest.formatVersion,
        manifest.revisionId,
        manifest.parentRevisionId,
        jsonString(json, "payload_hash_algorithm"),
        manifest.payloadSha256,
        manifest.parentRevisionIds};
    const BackupFormatValidation formatValidation =
        validateBackupFormatIdentity(formatIdentity);
    if (formatValidation != BackupFormatValidation::Valid) {
        error = backupFormatValidationMessage(formatValidation);
        return false;
    }
    if (!parseHex64(jsonString(json, "title_id"), manifest.titleId)
        || manifest.titleId == 0) {
        error = "Title ID is missing from the v2 manifest";
        return false;
    }
    manifest.titleName = jsonString(json, "title_name");
    manifest.gameVersion = jsonString(json, "game_version");
    manifest.sourceDevice = jsonString(json, "device_id");
    manifest.sourceProfile = jsonString(json, "profile_name");
    manifest.sourceProfileUid = jsonString(json, "profile_uid");
    manifest.createdUtc = jsonString(json, "created_utc");
    if (manifest.sourceDevice.empty()
        || manifest.sourceProfile.empty()
        || !isHexString(manifest.sourceProfileUid, 32)
        || manifest.createdUtc.empty()) {
        error = "Incomplete v2 backup source";
        return false;
    }
    number = 0;
    if (!jsonUnsigned(json, "file_count", number)) {
        error = "File count is missing from the v2 manifest";
        return false;
    }
    manifest.fileCount = static_cast<std::size_t>(number);
    number = 0;
    if (!jsonUnsigned(json, "uncompressed_bytes", number)) {
        error = "Payload size is missing from the v2 manifest";
        return false;
    }
    manifest.uncompressedBytes = number;

    if (!parseHex64(jsonString(json, "owner_id"), manifest.ownerId)) {
        error = "Invalid owner ID in the v2 manifest";
        return false;
    }
        number = 0;
        if (jsonUnsigned(json, "data_size", number)
            && number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            manifest.dataSize = static_cast<std::int64_t>(number);
        }
        number = 0;
        if (jsonUnsigned(json, "journal_size", number)
            && number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            manifest.journalSize = static_cast<std::int64_t>(number);
        }
        number = 0;
        if (jsonUnsigned(json, "save_flags", number)) {
            manifest.saveFlags = static_cast<std::uint32_t>(number);
        }
        number = 0;
        if (jsonUnsigned(json, "save_data_index", number)) {
            manifest.saveDataIndex = static_cast<std::uint16_t>(number);
        }
    manifest.valid = true;
    return true;
}

const SaveEntry* findDestinationSave(
    const UserSaves& user,
    const std::uint64_t titleId) {
    const auto found = std::find_if(user.saves.begin(), user.saves.end(), [&](const SaveEntry& save) {
        return save.applicationId == titleId;
    });
    return found == user.saves.end() ? nullptr : &*found;
}

bool ensureDirectories(const std::string& filePath, int& systemError) {
    const std::size_t deviceEnd = filePath.find(":/");
    if (deviceEnd == std::string::npos) {
        systemError = EINVAL;
        return false;
    }
    const std::size_t lastSlash = filePath.find_last_of('/');
    if (lastSlash == std::string::npos || lastSlash <= deviceEnd + 1) {
        return true;
    }
    for (std::size_t index = deviceEnd + 2; index <= lastSlash; ++index) {
        if (index != lastSlash && filePath[index] != '/') {
            continue;
        }
        const std::string directory = filePath.substr(0, index);
        if (mkdir(directory.c_str(), 0777) != 0 && errno != EEXIST) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

bool clearMountedSave(int& systemError) {
    DIR* directory = opendir(RestoreMountRoot);
    if (directory == nullptr) {
        systemError = errno;
        return false;
    }
    bool success = true;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string path = std::string(RestoreMountRoot) + name;
        struct stat item{};
        if (lstat(path.c_str(), &item) != 0) {
            systemError = errno;
            success = false;
            break;
        }
        if (S_ISDIR(item.st_mode)) {
            const Result deleteResult = fsdevDeleteDirectoryRecursively(path.c_str());
            if (R_FAILED(deleteResult)) {
                systemError = EIO;
                success = false;
                break;
            }
        } else if (std::remove(path.c_str()) != 0) {
            systemError = errno;
            success = false;
            break;
        }
    }
    closedir(directory);
    return success;
}

bool verifyFileCrc(const std::string& path, const std::uint32_t expectedCrc) {
    FILE* input = std::fopen(path.c_str(), "rb");
    if (input == nullptr) {
        return false;
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    std::vector<unsigned char> buffer(BufferSize);
    while (true) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count > 0) {
            crc = crc32(crc, buffer.data(), static_cast<uInt>(count));
        }
        if (count < buffer.size()) {
            break;
        }
    }
    const bool success = std::ferror(input) == 0
        && static_cast<std::uint32_t>(crc) == expectedCrc;
    std::fclose(input);
    return success;
}

bool extractArchive(
    const std::string& archivePath,
    const std::int64_t journalSize,
    const std::size_t totalFiles,
    const std::uint64_t totalBytes,
    RestoreResult& result,
    BackupProgressCallback progressCallback,
    void* progressContext) {
    unzFile archive = unzOpen64(archivePath.c_str());
    if (archive == nullptr || unzGoToFirstFile(archive) != UNZ_OK) {
        if (archive != nullptr) {
            unzClose(archive);
        }
        result.message = "Unable to reopen the verified ZIP archive";
        return false;
    }

    std::vector<unsigned char> buffer(BufferSize);
    BackupProgress progress;
    progress.stage = "Restoring to the destination profile";
    progress.totalFiles = totalFiles;
    progress.totalBytes = totalBytes;
    bool success = true;
    do {
        unz_file_info64 info{};
        std::array<char, 1024> nameBuffer{};
        if (unzGetCurrentFileInfo64(
                archive,
                &info,
                nameBuffer.data(),
                nameBuffer.size(),
                nullptr,
                0,
                nullptr,
                0) != UNZ_OK) {
            success = false;
            result.message = "ZIP entry is not readable during restore";
            break;
        }
        if (info.size_filename >= nameBuffer.size()) {
            success = false;
            result.message = "Filename is too long in the ZIP archive";
            break;
        }
        const std::string name(nameBuffer.data());
        std::string relativePath;
        if (!safeSaveEntryPath(name, relativePath)) {
            if (name == "nxsync-metadata.json") {
                continue;
            }
            success = false;
            result.message = "Unsafe path in the ZIP archive";
            break;
        }
        if (name == "nxsync-metadata.json") {
            continue;
        }
        if (name == "save/") {
            continue;
        }

        const bool directoryEntry = !relativePath.empty() && relativePath.back() == '/';
        const std::string destination = std::string(RestoreMountRoot) + relativePath;
        if (directoryEntry) {
            int directoryError = 0;
            if (!ensureDirectories(destination + "placeholder", directoryError)) {
                result.systemError = directoryError;
                result.message = "Unable to create a folder in the save";
                success = false;
                break;
            }
            continue;
        }

        int directoryError = 0;
        if (!ensureDirectories(destination, directoryError)) {
            result.systemError = directoryError;
            result.message = "Unable to prepare a save folder";
            success = false;
            break;
        }
        if (unzOpenCurrentFile(archive) != UNZ_OK) {
            result.message = "Unable to extract a file from the ZIP archive";
            success = false;
            break;
        }
        FILE* output = std::fopen(destination.c_str(), "wb");
        if (output == nullptr) {
            result.systemError = errno;
            result.message = "Unable to create a file in the save";
            unzCloseCurrentFile(archive);
            success = false;
            break;
        }

        std::int64_t sinceCommit = 0;
        std::uint64_t fileOffset = 0;
        while (success) {
            const int read = unzReadCurrentFile(archive, buffer.data(), buffer.size());
            if (read < 0) {
                result.message = "Error while extracting the ZIP archive";
                success = false;
                break;
            }
            if (read == 0) {
                break;
            }
            if (journalSize > 0 && sinceCommit > 0
                && sinceCommit + read >= journalSize) {
                if (std::fclose(output) != 0) {
                    output = nullptr;
                    result.systemError = errno;
                    result.message = "Unable to close a file during commit";
                    success = false;
                    break;
                }
                output = nullptr;
                result.systemResult = fsdevCommitDevice(RestoreMountName);
                if (R_FAILED(result.systemResult)) {
                    result.message = "Intermediate save commit failed";
                    success = false;
                    break;
                }
                output = std::fopen(destination.c_str(), "r+b");
                if (output == nullptr
                    || std::fseek(output, static_cast<long>(fileOffset), SEEK_SET) != 0) {
                    result.systemError = errno;
                    result.message = "Unable to resume writing the file";
                    success = false;
                    break;
                }
                sinceCommit = 0;
            }
            const std::size_t written = std::fwrite(
                buffer.data(),
                1,
                static_cast<std::size_t>(read),
                output);
            if (written != static_cast<std::size_t>(read)) {
                result.systemError = errno;
                result.message = "Incomplete write to the save";
                success = false;
                break;
            }
            fileOffset += written;
            sinceCommit += static_cast<std::int64_t>(written);
            progress.currentPath = relativePath;
            progress.bytesProcessed += written;
            if (progressCallback != nullptr) {
                progressCallback(progress, progressContext);
            }
        }
        if (output != nullptr && std::fclose(output) != 0 && success) {
            result.systemError = errno;
            result.message = "Failed to close the restored file";
            success = false;
        }
        const int closeZipEntry = unzCloseCurrentFile(archive);
        if (closeZipEntry != UNZ_OK && success) {
            result.message = "Invalid ZIP CRC during restore";
            success = false;
        }
        if (!success) {
            break;
        }

        result.systemResult = fsdevCommitDevice(RestoreMountName);
        if (R_FAILED(result.systemResult)) {
            result.message = "Restored file commit failed";
            success = false;
            break;
        }
        if (!verifyFileCrc(destination, static_cast<std::uint32_t>(info.crc))) {
            result.message = "Restored file verification failed";
            success = false;
            break;
        }
        ++result.restoredFiles;
        result.restoredBytes += info.uncompressed_size;
        progress.filesProcessed = result.restoredFiles;
    } while (unzGoToNextFile(archive) == UNZ_OK);

    unzClose(archive);
    return success;
}

void recoverMountedDestination(
    const std::int64_t journalSize,
    RestoreResult& result,
    BackupProgressCallback progressCallback,
    void* progressContext) {
    result.recoveryAttempted = true;

    int clearError = 0;
    const bool cleared = clearMountedSave(clearError);
    const bool clearCommitted = cleared
        && R_SUCCEEDED(fsdevCommitDevice(RestoreMountName));
    if (!clearCommitted) {
        result.recoverySucceeded = false;
        result.message += ". Automatic recovery failed during cleanup";
        return;
    }

    if (result.safetyBackupPath.empty()) {
        result.recoverySucceeded = true;
        result.message += ". The container had no previous data and was left empty";
        return;
    }

    const RestoreInspection safetyInspection = inspectRestoreArchive(
        result.safetyBackupPath);
    if (!safetyInspection.success) {
        result.recoverySucceeded = false;
        result.message += ". Invalid safety backup; retained at "
            + result.safetyBackupPath;
        return;
    }

    RestoreResult rollback;
    const bool extracted = extractArchive(
        result.safetyBackupPath,
        journalSize,
        safetyInspection.validatedFiles,
        safetyInspection.validatedBytes,
        rollback,
        progressCallback,
        progressContext);
    const bool finalCommitted = extracted
        && R_SUCCEEDED(fsdevCommitDevice(RestoreMountName));
    result.recoverySucceeded = extracted && finalCommitted;
    result.safetyBackupRestored = result.recoverySucceeded;
    result.message += result.recoverySucceeded
        ? ". Original save restored automatically"
        : ". Automatic restore failed; safety backup: "
            + result.safetyBackupPath;
}

} // namespace

RestoreInspection inspectRestoreArchive(const std::string& archivePath) {
    RestoreInspection inspection;
    unzFile archive = unzOpen64(archivePath.c_str());
    if (archive == nullptr) {
        inspection.message = "The downloaded backup is not a valid ZIP archive";
        return inspection;
    }
    if (unzGoToFirstFile(archive) != UNZ_OK) {
        unzClose(archive);
        inspection.message = "The backup ZIP archive is empty";
        return inspection;
    }

    bool manifestFound = false;
    bool saveContentFound = false;
    std::uint64_t totalBytes = 0;
    std::size_t totalFiles = 0;
    bool valid = true;
    std::vector<PayloadFileDigest> payloadFiles;
    std::set<std::string> payloadPaths;
    std::vector<unsigned char> buffer(BufferSize);
    do {
        unz_file_info64 info{};
        std::array<char, 1024> nameBuffer{};
        if (unzGetCurrentFileInfo64(
                archive,
                &info,
                nameBuffer.data(),
                nameBuffer.size(),
                nullptr,
                0,
                nullptr,
                0) != UNZ_OK) {
            inspection.message = "ZIP index is not readable";
            valid = false;
            break;
        }
        if (info.size_filename >= nameBuffer.size()) {
            inspection.message = "Filename is too long in the ZIP archive";
            valid = false;
            break;
        }
        const std::string name(nameBuffer.data());
        std::string relativePath;
        if (!safeSaveEntryPath(name, relativePath)) {
            inspection.message = "The backup contains an unsafe path";
            valid = false;
            break;
        }
        if (name == "nxsync-metadata.json") {
            if (manifestFound) {
                inspection.message = "The backup contains multiple NXSync manifests";
                valid = false;
                break;
            }
            std::string manifestJson;
            std::string manifestError;
            if (!readCurrentZipEntry(archive, manifestJson, 64 * 1024)
                || !parseManifest(
                    manifestJson,
                    inspection.manifest,
                    manifestError)) {
                inspection.message = manifestError.empty()
                    ? "NXSync manifest is missing or invalid"
                    : manifestError;
                valid = false;
                break;
            }
            manifestFound = true;
            continue;
        }
        if (name == "save/") {
            continue;
        }
        if (!relativePath.empty() && relativePath.back() == '/') {
            continue;
        }
        if (!payloadPaths.insert(relativePath).second) {
            inspection.message = "The backup contains duplicate paths";
            valid = false;
            break;
        }

        saveContentFound = true;
        if (++totalFiles > MaximumFiles
            || info.uncompressed_size > MaximumArchiveBytes - totalBytes) {
            inspection.message = "The backup exceeds safety limits";
            valid = false;
            break;
        }
        totalBytes += info.uncompressed_size;
        if (unzOpenCurrentFile(archive) != UNZ_OK) {
            inspection.message = "Unable to verify a file in the ZIP archive";
            valid = false;
            break;
        }
        Sha256Context fileHashContext{};
        sha256ContextCreate(&fileHashContext);
        while (true) {
            const int read = unzReadCurrentFile(archive, buffer.data(), buffer.size());
            if (read < 0) {
                valid = false;
                inspection.message = "Error during ZIP CRC verification";
                break;
            }
            if (read == 0) {
                break;
            }
            sha256ContextUpdate(
                &fileHashContext,
                buffer.data(),
                static_cast<std::size_t>(read));
        }
        if (unzCloseCurrentFile(archive) != UNZ_OK) {
            valid = false;
            inspection.message = "Invalid CRC in the backup";
        }
        if (!valid) {
            break;
        }
        PayloadFileDigest payloadFile;
        payloadFile.path = relativePath;
        payloadFile.size = info.uncompressed_size;
        sha256ContextGetHash(&fileHashContext, payloadFile.digest.data());
        payloadFiles.push_back(std::move(payloadFile));
    } while (unzGoToNextFile(archive) == UNZ_OK);
    unzClose(archive);

    if (!valid) {
        return inspection;
    }
    if (!manifestFound || !saveContentFound) {
        inspection.message = !manifestFound
            ? "NXSync manifest was not found"
            : "The backup contains no save files";
        return inspection;
    }
    if ((inspection.manifest.fileCount != 0
            && inspection.manifest.fileCount != totalFiles)
        || (inspection.manifest.uncompressedBytes != 0
            && inspection.manifest.uncompressedBytes != totalBytes)) {
        inspection.message = "ZIP contents differ from the manifest";
        return inspection;
    }
    if (calculatePayloadSha256(std::move(payloadFiles))
        != inspection.manifest.payloadSha256) {
        inspection.message = "Payload hash differs from the v2 manifest";
        return inspection;
    }
    inspection.success = true;
    inspection.validatedFiles = totalFiles;
    inspection.validatedBytes = totalBytes;
    inspection.message = "NXSync backup verified";
    return inspection;
}

RestoreResult restoreArchiveToProfile(
    const std::string& archivePath,
    const RestoreInspection& inspection,
    const DeviceIdentity& destinationIdentity,
    const UserSaves& destinationUser,
    BackupProgressCallback progressCallback,
    void* progressContext) {
    RestoreResult result;
    if (!inspection.success || !inspection.manifest.valid) {
        result.message = "The backup did not pass verification";
        return result;
    }

    const RestoreManifest& manifest = inspection.manifest;
    const ApplicationSaveDataDefaults defaults = loadApplicationSaveDataDefaults(
        manifest.titleId);
    if (!defaults.available) {
        result.systemResult = defaults.result;
        result.message = "The game is not installed or its metadata is unavailable";
        return result;
    }
    if (compareGameVersions(defaults.gameVersion, manifest.gameVersion)
        == GameVersionOrder::Older) {
        result.message = "Installed version " + defaults.gameVersion
            + " is older than backup version " + manifest.gameVersion;
        return result;
    }

    const SaveEntry* existingSave = findDestinationSave(destinationUser, manifest.titleId);
    FsSaveDataAttribute createdAttributes{};
    if (existingSave != nullptr) {
        BackupProgress safetyProgress;
        safetyProgress.stage = "Creating destination safety backup";
        if (progressCallback != nullptr) {
            progressCallback(safetyProgress, progressContext);
        }
        const BackupResult safety = createLocalBackup(
            destinationIdentity,
            destinationUser,
            *existingSave,
            progressCallback,
            progressContext);
        if (!safety.success && !safety.emptySave) {
            result.systemResult = safety.mountResult;
            result.systemError = safety.systemError;
            result.message = "Safety backup failed: " + safety.message;
            return result;
        }
        if (safety.success) {
            result.safetyBackupPath = safety.archivePath;
        }

        const std::int64_t requiredDataSize = alignSaveSize(std::max<std::int64_t>(
            manifest.dataSize,
            static_cast<std::int64_t>(inspection.validatedBytes)));
        const std::int64_t requiredJournalSize = alignSaveSize(std::max<std::int64_t>(
                manifest.journalSize,
                existingSave->journalSize));
        if (requiredDataSize > existingSave->dataSize
            || requiredJournalSize > existingSave->journalSize) {
            result.systemResult = fsExtendSaveDataFileSystem(
                existingSave->saveDataSpaceId,
                existingSave->saveDataId,
                std::max(requiredDataSize, existingSave->dataSize),
                std::max(requiredJournalSize, existingSave->journalSize));
            if (R_FAILED(result.systemResult)) {
                result.message = "Unable to extend the destination container";
                return result;
            }
        }
    } else {
        std::int64_t dataSize = manifest.dataSize > 0
            ? manifest.dataSize
            : static_cast<std::int64_t>(defaults.dataSize);
        if (dataSize <= 0) {
            dataSize = static_cast<std::int64_t>(defaults.dataSizeMax);
        }
        dataSize = alignSaveSize(std::max<std::int64_t>(
            dataSize,
            static_cast<std::int64_t>(inspection.validatedBytes)));
        std::int64_t journalSize = manifest.journalSize > 0
            ? manifest.journalSize
            : static_cast<std::int64_t>(defaults.journalSize);
        if (journalSize <= 0) {
            journalSize = static_cast<std::int64_t>(defaults.journalSizeMax);
        }
        journalSize = alignSaveSize(journalSize);
        if (dataSize <= 0 || journalSize <= 0) {
            result.message = "New save size is unavailable";
            return result;
        }

        createdAttributes.application_id = manifest.titleId;
        createdAttributes.uid = destinationUser.uid;
        createdAttributes.save_data_type = FsSaveDataType_Account;
        createdAttributes.save_data_rank = FsSaveDataRank_Primary;
        createdAttributes.save_data_index = manifest.saveDataIndex;

        FsSaveDataCreationInfo creation{};
        creation.save_data_size = dataSize;
        creation.journal_size = journalSize;
        creation.available_size = 0x4000;
        creation.owner_id = defaults.ownerId != 0 ? defaults.ownerId : manifest.ownerId;
        creation.flags = manifest.saveFlags;
        creation.save_data_space_id = FsSaveDataSpaceId_User;

        FsSaveDataMetaInfo meta{};
        meta.size = 0x40060;
        meta.type = FsSaveDataMetaType_Thumbnail;
        result.systemResult = fsCreateSaveDataFileSystem(
            &createdAttributes,
            &creation,
            &meta);
        if (R_FAILED(result.systemResult)) {
            result.message = "Failed to create the save for the destination profile";
            return result;
        }
        result.createdContainer = true;
        result.destinationModified = true;
    }

    result.systemResult = fsdevMountSaveData(
        RestoreMountName,
        manifest.titleId,
        destinationUser.uid);
    if (R_FAILED(result.systemResult)) {
        result.message = "Failed to mount the save for writing";
        if (result.createdContainer) {
            result.recoveryAttempted = true;
            result.recoverySucceeded = R_SUCCEEDED(
                fsDeleteSaveDataFileSystemBySaveDataAttribute(
                    FsSaveDataSpaceId_User,
                    &createdAttributes));
            result.message += result.recoverySucceeded
                ? ". The newly created container was removed"
                : ". Unable to remove the newly created container";
        }
        return result;
    }
    MountedRestoreSave mountGuard(true);

    const std::int64_t journalSize = existingSave != nullptr
        ? std::max<std::int64_t>(
            existingSave->journalSize,
            manifest.journalSize > 0
                ? manifest.journalSize
                : static_cast<std::int64_t>(defaults.journalSize))
        : (manifest.journalSize > 0
            ? manifest.journalSize
            : static_cast<std::int64_t>(defaults.journalSize));

    const auto recoverDestination = [&]() {
        recoverMountedDestination(
            journalSize,
            result,
            progressCallback,
            progressContext);
        if (!result.createdContainer) {
            return;
        }
        mountGuard.unmount();
        const bool removed = R_SUCCEEDED(
            fsDeleteSaveDataFileSystemBySaveDataAttribute(
                FsSaveDataSpaceId_User,
                &createdAttributes));
        result.recoverySucceeded = result.recoverySucceeded && removed;
        result.message += removed
            ? ". The newly created container was removed"
            : ". Unable to remove the newly created container";
    };

    result.destinationModified = true;
    if (!clearMountedSave(result.systemError)) {
        result.message = "Unable to clear the destination save";
        recoverDestination();
        return result;
    }
    result.systemResult = fsdevCommitDevice(RestoreMountName);
    if (R_FAILED(result.systemResult)) {
        result.message = "Save cleanup commit failed";
        recoverDestination();
        return result;
    }

    if (!extractArchive(
            archivePath,
            journalSize,
            inspection.validatedFiles,
            inspection.validatedBytes,
            result,
            progressCallback,
            progressContext)) {
        recoverDestination();
        return result;
    }
    result.systemResult = fsdevCommitDevice(RestoreMountName);
    if (R_FAILED(result.systemResult)) {
        result.message = "Final save commit failed";
        recoverDestination();
        return result;
    }

    result.success = true;
    result.message = "Save restored to profile " + destinationUser.nickname;
    result.lineageRecorded = writeRestoreLineageAnchor(
        destinationIdentity,
        destinationUser,
        manifest.titleId,
        manifest.revisionId,
        manifest.payloadSha256,
        {},
        result.lineageSystemError);
    if (!result.lineageRecorded) {
        result.message += ". Cloud continuity was not recorded (errno "
            + std::to_string(result.lineageSystemError) + ")";
    }
    result.diagnostic = "Source "
        + (manifest.sourceDevice.empty() ? std::string("unknown") : manifest.sourceDevice)
        + " / "
        + (manifest.sourceProfile.empty() ? std::string("unknown profile") : manifest.sourceProfile)
        + "; destination " + destinationUser.nickname
        + "; backup version "
        + (manifest.gameVersion.empty() ? std::string("not recorded") : manifest.gameVersion)
        + "; installed "
        + (defaults.gameVersion.empty() ? std::string("unknown") : defaults.gameVersion);
    return result;
}

} // namespace nxsync
