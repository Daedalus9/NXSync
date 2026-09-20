#include "nxsync/backup_manager.hpp"

#include "nxsync/backup_format.hpp"
#include "nxsync/backup_state.hpp"
#include "nxsync/revision_parents.hpp"
#include "nxsync/remote_layout.hpp"

#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nxsync {
namespace {

constexpr const char* MountName = "nxsyncsave";
constexpr const char* MountRoot = "nxsyncsave:/";
constexpr const char* LocalBackupRoot = "sdmc:/switch/NXSync/backups";
#ifdef NXSYNC_BACKUP_BUFFER_SIZE
constexpr std::size_t CopyBufferSize = NXSYNC_BACKUP_BUFFER_SIZE;
#else
constexpr std::size_t CopyBufferSize = 128 * 1024;
#endif
#ifdef NXSYNC_ZIP_MEM_LEVEL
constexpr int ZipMemoryLevel = NXSYNC_ZIP_MEM_LEVEL;
#else
constexpr int ZipMemoryLevel = DEF_MEM_LEVEL;
#endif
#ifdef NXSYNC_ZIP_STORE_ONLY
constexpr int ZipCompressionMethod = 0;
constexpr int ZipCompressionLevel = 0;
#else
constexpr int ZipCompressionMethod = Z_DEFLATED;
constexpr int ZipCompressionLevel = Z_BEST_SPEED;
#endif
constexpr std::uint64_t ProgressInterval = 1ULL * 1024ULL * 1024ULL;

class MountedSave {
public:
    explicit MountedSave(const bool mounted) : mounted_(mounted) {}
    ~MountedSave() {
        if (mounted_) {
            fsdevUnmountDevice(MountName);
        }
    }

    MountedSave(const MountedSave&) = delete;
    MountedSave& operator=(const MountedSave&) = delete;

private:
    bool mounted_{false};
};

struct ArchiveContext {
    zipFile archive{nullptr};
    BackupProgress progress;
    BackupProgressCallback callback{nullptr};
    void* callbackContext{nullptr};
    std::string error;
    int systemError{0};
    struct PayloadFileDigest {
        std::string path;
        std::uint64_t size{0};
        std::array<unsigned char, SHA256_HASH_SIZE> digest{};
    };
    std::vector<PayloadFileDigest> payloadFiles;
};

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

std::string calculatePayloadSha256(
    std::vector<ArchiveContext::PayloadFileDigest> files) {
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    Sha256Context context{};
    sha256ContextCreate(&context);
    for (const auto& file : files) {
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

std::string calculateTextSha256(const std::string& value) {
    Sha256Context context{};
    sha256ContextCreate(&context);
    sha256ContextUpdate(&context, value.data(), value.size());
    std::array<unsigned char, SHA256_HASH_SIZE> hash{};
    sha256ContextGetHash(&context, hash.data());
    return hashToHex(hash);
}

void reportProgress(ArchiveContext& context) {
    if (context.callback != nullptr) {
        context.callback(context.progress, context.callbackContext);
    }
}

std::string fallbackSegment(const std::string& value, const char* fallback) {
    const std::string sanitized = sanitizePathSegment(value, false);
    return sanitized.empty() ? fallback : sanitized;
}

std::string formatUtcTimestamp(const std::time_t timestamp) {
    std::tm utc{};
    if (gmtime_r(&timestamp, &utc) == nullptr) {
        return "unknown-time";
    }

    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H%M%SZ", &utc) == 0) {
        return "unknown-time";
    }
    return buffer;
}

std::time_t getBackupTimestamp(std::string& clockSource) {
    u64 timestamp = 0;
    const auto fitsTimeT = [](const u64 value) {
        return value <= static_cast<u64>(std::numeric_limits<std::time_t>::max());
    };

    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_NetworkSystemClock, &timestamp))
        && fitsTimeT(timestamp)) {
        clockSource = "network";
        return static_cast<std::time_t>(timestamp);
    }
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp))
        && fitsTimeT(timestamp)) {
        clockSource = "local-system";
        return static_cast<std::time_t>(timestamp);
    }
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &timestamp))
        && fitsTimeT(timestamp)) {
        clockSource = "user";
        return static_cast<std::time_t>(timestamp);
    }

    clockSource = "posix-fallback";
    return std::time(nullptr);
}

std::string formatUid(const AccountUid& uid) {
    char buffer[33]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llX%016llX",
        static_cast<unsigned long long>(uid.uid[1]),
        static_cast<unsigned long long>(uid.uid[0]));
    return buffer;
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[7]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", ch);
                    escaped += buffer;
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

bool createDirectories(const std::string& path, int& systemError) {
    const std::size_t deviceEnd = path.find(":/");
    if (deviceEnd == std::string::npos) {
        systemError = EINVAL;
        return false;
    }

    for (std::size_t index = deviceEnd + 2; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/') {
            continue;
        }

        const std::string current = path.substr(0, index);
        if (current.empty()) {
            continue;
        }
        if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

bool scanTree(ArchiveContext& context, const std::string& sourceDirectory) {
    DIR* directory = opendir(sourceDirectory.c_str());
    if (directory == nullptr) {
        context.systemError = errno;
        context.error = "Unable to scan a save folder";
        return false;
    }
    std::vector<std::string> names;
    errno = 0;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    const int readError = errno;
    closedir(directory);
    if (readError != 0) {
        context.systemError = readError;
        context.error = "Error while scanning the save";
        return false;
    }

    for (const std::string& name : names) {
        const std::string path = sourceDirectory
            + (sourceDirectory.back() == '/' ? "" : "/") + name;
        struct stat fileStat{};
        if (stat(path.c_str(), &fileStat) != 0) {
            context.systemError = errno;
            context.error = "Unable to measure a save file";
            return false;
        }
        if (S_ISDIR(fileStat.st_mode)) {
            if (!scanTree(context, path)) {
                return false;
            }
        } else if (S_ISREG(fileStat.st_mode)) {
            const std::uint64_t size = fileStat.st_size > 0
                ? static_cast<std::uint64_t>(fileStat.st_size)
                : 0;
            if (size > std::numeric_limits<std::uint64_t>::max()
                    - context.progress.totalBytes
                || context.progress.totalFiles
                    == std::numeric_limits<std::size_t>::max()) {
                context.error = "Save size cannot be represented";
                return false;
            }
            context.progress.totalBytes += size;
            ++context.progress.totalFiles;
        }
    }
    return true;
}

zip_fileinfo makeZipInfo(const std::time_t timestamp, const mode_t mode) {
    zip_fileinfo info{};
    std::tm utc{};
    if (gmtime_r(&timestamp, &utc) != nullptr) {
        info.tmz_date.tm_sec = utc.tm_sec;
        info.tmz_date.tm_min = utc.tm_min;
        info.tmz_date.tm_hour = utc.tm_hour;
        info.tmz_date.tm_mday = utc.tm_mday;
        info.tmz_date.tm_mon = utc.tm_mon;
        info.tmz_date.tm_year = std::max(1980, std::min(2044, utc.tm_year + 1900));
    } else {
        info.tmz_date.tm_mday = 1;
        info.tmz_date.tm_mon = 0;
        info.tmz_date.tm_year = 1980;
    }
    info.external_fa = static_cast<uLong>((mode & 0xFFFFU) << 16U);
    return info;
}

bool addDirectoryEntry(
    ArchiveContext& context,
    const std::string& relativePath,
    const struct stat& fileStat) {
    const std::string archivePath = relativePath + "/";
    const zip_fileinfo info = makeZipInfo(fileStat.st_mtime, fileStat.st_mode);
    int zipResult = zipOpenNewFileInZip64(
        context.archive,
        archivePath.c_str(),
        &info,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0,
        0,
        0);
    if (zipResult == ZIP_OK) {
        zipResult = zipCloseFileInZip(context.archive);
    }
    if (zipResult != ZIP_OK) {
        context.error = "Unable to add a folder to the ZIP archive";
        return false;
    }
    return true;
}

bool addFile(
    ArchiveContext& context,
    const std::string& sourcePath,
    const std::string& relativePath,
    const struct stat& fileStat) {
    FILE* input = std::fopen(sourcePath.c_str(), "rb");
    if (input == nullptr) {
        context.systemError = errno;
        context.error = "Unable to read a save file";
        return false;
    }

    const zip_fileinfo info = makeZipInfo(fileStat.st_mtime, fileStat.st_mode);
    const bool needsZip64 = static_cast<std::uint64_t>(fileStat.st_size) >= 0xFFFFFFFFULL;
    context.progress.currentPath = relativePath;
    reportProgress(context);
    int zipResult = zipOpenNewFileInZip3_64(
        context.archive,
        relativePath.c_str(),
        &info,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        ZipCompressionMethod,
        ZipCompressionLevel,
        0,
        -MAX_WBITS,
        ZipMemoryLevel,
        Z_DEFAULT_STRATEGY,
        nullptr,
        0,
        needsZip64 ? 1 : 0);
    if (zipResult != ZIP_OK) {
        std::fclose(input);
        context.error = "Unable to create a ZIP entry";
        return false;
    }

    Sha256Context fileHashContext{};
    sha256ContextCreate(&fileHashContext);
    std::vector<unsigned char> buffer(CopyBufferSize);
    std::uint64_t bytesSinceProgress = 0;
    bool succeeded = true;
    while (true) {
        const std::size_t bytesRead = std::fread(buffer.data(), 1, buffer.size(), input);
        if (bytesRead > 0) {
            sha256ContextUpdate(&fileHashContext, buffer.data(), bytesRead);
            zipResult = zipWriteInFileInZip(
                context.archive,
                buffer.data(),
                static_cast<unsigned>(bytesRead));
            if (zipResult != ZIP_OK) {
                context.error = "Error while writing the ZIP archive";
                succeeded = false;
                break;
            }
            context.progress.bytesProcessed += bytesRead;
            bytesSinceProgress += bytesRead;
            if (bytesSinceProgress >= ProgressInterval) {
                reportProgress(context);
                bytesSinceProgress = 0;
            }
        }

        if (bytesRead < buffer.size()) {
            if (std::ferror(input) != 0) {
                context.systemError = errno;
                context.error = "Error while reading the save";
                succeeded = false;
            }
            break;
        }
    }

    std::fclose(input);
    const int closeResult = zipCloseFileInZip(context.archive);
    if (closeResult != ZIP_OK && succeeded) {
        context.error = "Unable to close a ZIP entry";
        succeeded = false;
    }
    if (succeeded) {
        ArchiveContext::PayloadFileDigest payloadFile;
        payloadFile.path = relativePath.compare(0, 5, "save/") == 0
            ? relativePath.substr(5)
            : relativePath;
        payloadFile.size = fileStat.st_size > 0
            ? static_cast<std::uint64_t>(fileStat.st_size)
            : 0;
        sha256ContextGetHash(&fileHashContext, payloadFile.digest.data());
        context.payloadFiles.push_back(std::move(payloadFile));
        ++context.progress.filesProcessed;
        reportProgress(context);
    }
    return succeeded;
}

bool addTree(
    ArchiveContext& context,
    const std::string& sourceDirectory,
    const std::string& relativeDirectory) {
    DIR* directory = opendir(sourceDirectory.c_str());
    if (directory == nullptr) {
        context.systemError = errno;
        context.error = "Unable to open a save folder";
        return false;
    }

    std::vector<std::string> names;
    errno = 0;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    const int readError = errno;
    closedir(directory);
    if (readError != 0) {
        context.systemError = readError;
        context.error = "Error while reading a folder";
        return false;
    }
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        const std::string sourcePath = sourceDirectory + (sourceDirectory.back() == '/' ? "" : "/") + name;
        const std::string relativePath = relativeDirectory.empty()
            ? name
            : relativeDirectory + "/" + name;

        struct stat fileStat{};
        if (stat(sourcePath.c_str(), &fileStat) != 0) {
            context.systemError = errno;
            context.error = "Unable to read file information";
            return false;
        }

        if (S_ISDIR(fileStat.st_mode)) {
            if (!addDirectoryEntry(context, relativePath, fileStat)
                || !addTree(context, sourcePath, relativePath)) {
                return false;
            }
        } else if (S_ISREG(fileStat.st_mode)) {
            if (!addFile(context, sourcePath, relativePath, fileStat)) {
                return false;
            }
        }
    }
    return true;
}

bool addTextEntry(
    zipFile archive,
    const std::string& archivePath,
    const std::string& text,
    const std::time_t timestamp) {
    const zip_fileinfo info = makeZipInfo(timestamp, S_IFREG | 0644);
    int zipResult = zipOpenNewFileInZip3_64(
        archive,
        archivePath.c_str(),
        &info,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        ZipCompressionMethod,
        ZipCompressionLevel,
        0,
        -MAX_WBITS,
        ZipMemoryLevel,
        Z_DEFAULT_STRATEGY,
        nullptr,
        0,
        0);
    if (zipResult != ZIP_OK) {
        return false;
    }
    zipResult = zipWriteInFileInZip(
        archive,
        text.data(),
        static_cast<unsigned>(text.size()));
    const int closeResult = zipCloseFileInZip(archive);
    return zipResult == ZIP_OK && closeResult == ZIP_OK;
}

std::string makeManifest(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    const std::string& timestamp,
    const std::string& clockSource,
    const std::string& revisionId,
    const std::string& parentRevisionId,
    const std::vector<std::string>& parentRevisionIds,
    const std::string& payloadSha256,
    const BackupProgress& progress) {
    const std::string commitId = save.extraDataAvailable
        ? "\"" + formatTitleId(save.commitId) + "\""
        : "null";
    const std::string saveTimestamp = save.extraDataAvailable
        ? std::to_string(save.saveTimestamp)
        : "null";
    const std::vector<std::string> parents = normalizedRevisionParents(
        parentRevisionId, parentRevisionIds);
    const std::string parentRevision = parents.empty()
        ? "null"
        : "\"" + jsonEscape(parents.front()) + "\"";
    std::string parentRevisions = "[";
    for (std::size_t index = 0; index < parents.size(); ++index) {
        if (index != 0) parentRevisions += ", ";
        parentRevisions += "\"" + jsonEscape(parents[index]) + "\"";
    }
    parentRevisions += "]";
    return "{\n"
        "  \"schema\": \"" + std::string(BackupSchema) + "\",\n"
        "  \"format_version\": " + std::to_string(BackupFormatVersion) + ",\n"
        "  \"revision_id\": \"" + jsonEscape(revisionId) + "\",\n"
        "  \"parent_revision_id\": " + parentRevision + ",\n"
        "  \"parent_revision_ids\": " + parentRevisions + ",\n"
        "  \"payload_hash_algorithm\": \"" + std::string(PayloadHashAlgorithm) + "\",\n"
        "  \"payload_sha256\": \"" + jsonEscape(payloadSha256) + "\",\n"
        "  \"created_utc\": \"" + jsonEscape(timestamp) + "\",\n"
        "  \"clock_source\": \"" + jsonEscape(clockSource) + "\",\n"
        "  \"device_id\": \"" + jsonEscape(identity.folderName) + "\",\n"
        "  \"profile_name\": \"" + jsonEscape(user.nickname) + "\",\n"
        "  \"profile_uid\": \"" + formatUid(user.uid) + "\",\n"
        "  \"title_id\": \"" + formatTitleId(save.applicationId) + "\",\n"
        "  \"title_name\": \"" + jsonEscape(save.titleName) + "\",\n"
        "  \"game_version\": \"" + jsonEscape(save.gameVersion) + "\",\n"
        "  \"save_data_id\": \"" + formatTitleId(save.saveDataId) + "\",\n"
        "  \"save_data_space_id\": "
            + std::to_string(static_cast<unsigned>(save.saveDataSpaceId)) + ",\n"
        "  \"save_data_rank\": "
            + std::to_string(static_cast<unsigned>(save.saveDataRank)) + ",\n"
        "  \"save_data_index\": " + std::to_string(save.saveDataIndex) + ",\n"
        "  \"owner_id\": \"" + formatTitleId(save.ownerId) + "\",\n"
        "  \"data_size\": " + std::to_string(save.dataSize) + ",\n"
        "  \"journal_size\": " + std::to_string(save.journalSize) + ",\n"
        "  \"save_flags\": " + std::to_string(save.flags) + ",\n"
        "  \"commit_id\": " + commitId + ",\n"
        "  \"save_timestamp\": " + saveTimestamp + ",\n"
        "  \"file_count\": " + std::to_string(progress.filesProcessed) + ",\n"
        "  \"uncompressed_bytes\": " + std::to_string(progress.bytesProcessed) + "\n"
        "}\n";
}

bool calculateFileSha256(
    const std::string& path,
    std::string& hashHex,
    int& systemError) {
    FILE* input = std::fopen(path.c_str(), "rb");
    if (input == nullptr) {
        systemError = errno;
        return false;
    }

    Sha256Context context{};
    sha256ContextCreate(&context);
    std::vector<unsigned char> buffer(CopyBufferSize);
    while (true) {
        const std::size_t bytesRead = std::fread(buffer.data(), 1, buffer.size(), input);
        if (bytesRead > 0) {
            sha256ContextUpdate(&context, buffer.data(), bytesRead);
        }
        if (bytesRead < buffer.size()) {
            if (std::ferror(input) != 0) {
                systemError = errno;
                std::fclose(input);
                return false;
            }
            break;
        }
    }
    std::fclose(input);

    std::array<unsigned char, SHA256_HASH_SIZE> hash{};
    sha256ContextGetHash(&context, hash.data());
    static constexpr char HexDigits[] = "0123456789abcdef";
    hashHex.clear();
    hashHex.reserve(hash.size() * 2);
    for (const unsigned char byte : hash) {
        hashHex.push_back(HexDigits[byte >> 4U]);
        hashHex.push_back(HexDigits[byte & 0x0FU]);
    }
    return true;
}

bool verifyZipArchive(const std::string& path, std::string& error) {
    unzFile archive = unzOpen64(path.c_str());
    if (archive == nullptr) {
        error = "The created ZIP file cannot be reopened";
        return false;
    }

    std::vector<unsigned char> buffer(CopyBufferSize);
    int navigationResult = unzGoToFirstFile(archive);
    bool succeeded = navigationResult == UNZ_OK;
    while (succeeded && navigationResult == UNZ_OK) {
        if (unzOpenCurrentFile(archive) != UNZ_OK) {
            error = "Unable to verify a ZIP entry";
            succeeded = false;
            break;
        }

        int bytesRead = 0;
        do {
            bytesRead = unzReadCurrentFile(
                archive,
                buffer.data(),
                static_cast<unsigned>(buffer.size()));
        } while (bytesRead > 0);

        const int closeResult = unzCloseCurrentFile(archive);
        if (bytesRead < 0 || closeResult != UNZ_OK) {
            error = "ZIP CRC verification failed";
            succeeded = false;
            break;
        }
        navigationResult = unzGoToNextFile(archive);
    }

    if (succeeded && navigationResult != UNZ_END_OF_LIST_OF_FILE) {
        error = "Invalid ZIP central directory";
        succeeded = false;
    }
    if (unzClose(archive) != UNZ_OK && succeeded) {
        error = "Unable to close the verified ZIP archive";
        succeeded = false;
    }
    return succeeded;
}

bool fileExists(const std::string& path) {
    struct stat fileStat{};
    return stat(path.c_str(), &fileStat) == 0 && S_ISREG(fileStat.st_mode);
}

} // namespace

BackupResult createLocalBackup(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    BackupProgressCallback progressCallback,
    void* progressContext) {
    BackupResult result;
    const LocalBackupState previousState = findCurrentLocalBackup(identity, user, save);
    const RestoreLineageAnchor restoreAnchor = loadRestoreLineageAnchor(
        identity,
        user,
        save.applicationId);
    result.parentRevisionIds = selectBackupParentRevisions(
        previousState.recordValid,
        previousState.revisionId,
        restoreAnchor);
    result.parentRevisionId = result.parentRevisionIds.empty()
        ? std::string()
        : result.parentRevisionIds.front();
    ArchiveContext archiveContext;
    archiveContext.callback = progressCallback;
    archiveContext.callbackContext = progressContext;
    archiveContext.progress.stage = "Mounting save as read-only";
    reportProgress(archiveContext);

    FsSaveDataAttribute attribute{};
    attribute.application_id = save.applicationId;
    attribute.uid = user.uid;
    attribute.save_data_type = FsSaveDataType_Account;
    attribute.save_data_rank = save.saveDataRank;
    attribute.save_data_index = save.saveDataIndex;

    FsFileSystem saveFileSystem{};
    result.mountResult = fsOpenReadOnlySaveDataFileSystem(
        &saveFileSystem,
        save.saveDataSpaceId,
        &attribute);
    if (R_FAILED(result.mountResult)) {
        result.message = "Unable to open the save as read-only";
        return result;
    }
    if (fsdevMountDevice(MountName, saveFileSystem) < 0) {
        result.mountResult = fsdevGetLastResult();
        result.message = "Unable to mount the save";
        return result;
    }
    const MountedSave mountedSave(true);

    const std::string deviceFolder = fallbackSegment(identity.folderName, "NS-UNKNOWN");
    const std::string profileFolder = makeProfileBackupFolder(formatUid(user.uid));
    const std::string titleFolder = formatTitleId(save.applicationId);
    const std::string destinationDirectory = std::string(LocalBackupRoot)
        + "/" + deviceFolder
        + "/" + profileFolder
        + "/" + titleFolder;
    if (!createDirectories(destinationDirectory, result.systemError)) {
        result.message = "Unable to create the backup folder on the microSD card";
        return result;
    }

    archiveContext.progress.stage = "Scanning save contents";
    archiveContext.progress.currentPath.clear();
    reportProgress(archiveContext);
    if (!scanTree(archiveContext, MountRoot)) {
        result.systemError = archiveContext.systemError;
        result.message = archiveContext.error;
        return result;
    }
    reportProgress(archiveContext);
    if (archiveContext.progress.totalFiles == 0) {
        result.emptySave = true;
        result.message = "No save data for this profile";
        int stateError = 0;
        if (!writeLocalEmptySaveState(identity, user, save, stateError)) {
            result.message += " (local status was not updated)";
        }
        return result;
    }

    std::string clockSource;
    const std::time_t now = getBackupTimestamp(clockSource);
    const std::string timestamp = formatUtcTimestamp(now);
    char saveIdBuffer[17]{};
    std::snprintf(
        saveIdBuffer,
        sizeof(saveIdBuffer),
        "%016llX",
        static_cast<unsigned long long>(save.saveDataId));
    const std::string temporaryPath = destinationDirectory
        + "/." + timestamp + "_" + saveIdBuffer + ".partial.zip";

    archiveContext.progress.stage = "Creating ZIP archive";
    reportProgress(archiveContext);
    archiveContext.archive = zipOpen64(temporaryPath.c_str(), APPEND_STATUS_CREATE);
    if (archiveContext.archive == nullptr) {
        result.systemError = errno;
        result.message = "Unable to create the ZIP file on the microSD card";
        return result;
    }

    struct stat rootStat{};
    rootStat.st_mode = S_IFDIR | 0755;
    rootStat.st_mtime = now;
    bool archiveSucceeded = addDirectoryEntry(archiveContext, "save", rootStat)
        && addTree(archiveContext, MountRoot, "save");
    if (archiveSucceeded && archiveContext.progress.filesProcessed == 0) {
        result.emptySave = true;
        archiveContext.error = "No save data for this profile";
        archiveSucceeded = false;
    }
    if (archiveSucceeded) {
        result.payloadSha256 = calculatePayloadSha256(archiveContext.payloadFiles);
        const std::string revisionSeed = result.payloadSha256 + "\n"
            + formatTitleId(save.applicationId) + "\n"
            + formatTitleId(save.saveDataId) + "\n"
            + identity.folderName + "\n"
            + formatUid(user.uid) + "\n"
            + timestamp + "\n"
            + (save.extraDataAvailable ? formatTitleId(save.commitId) : std::string())
            + "\n" + [&]() {
                std::string parents;
                for (const std::string& parent : result.parentRevisionIds) {
                    if (!parents.empty()) parents.push_back(',');
                    parents += parent;
                }
                return parents;
            }();
        result.revisionId = calculateTextSha256(revisionSeed);
        const std::string manifest = makeManifest(
            identity,
            user,
            save,
            timestamp,
            clockSource,
            result.revisionId,
            result.parentRevisionId,
            result.parentRevisionIds,
            result.payloadSha256,
            archiveContext.progress);
        archiveSucceeded = addTextEntry(
            archiveContext.archive,
            "nxsync-metadata.json",
            manifest,
            now);
        if (!archiveSucceeded) {
            archiveContext.error = "Unable to add metadata to the ZIP archive";
        }
    }

    const int closeResult = zipClose(archiveContext.archive, nullptr);
    archiveContext.archive = nullptr;
    if (closeResult != ZIP_OK && archiveSucceeded) {
        archiveContext.error = "Unable to finalize the ZIP archive";
        archiveSucceeded = false;
    }
    if (!archiveSucceeded) {
        std::remove(temporaryPath.c_str());
        result.systemError = archiveContext.systemError;
        result.message = archiveContext.error;
        if (result.emptySave) {
            int stateError = 0;
            if (!writeLocalEmptySaveState(identity, user, save, stateError)) {
                result.message += " (local status was not updated)";
            }
        }
        return result;
    }

    archiveContext.progress.stage = "Verifying ZIP and CRC";
    archiveContext.progress.currentPath.clear();
    reportProgress(archiveContext);
    if (!verifyZipArchive(temporaryPath, archiveContext.error)) {
        std::remove(temporaryPath.c_str());
        result.message = archiveContext.error;
        return result;
    }

    archiveContext.progress.stage = "Calculating SHA-256";
    reportProgress(archiveContext);
    if (!calculateFileSha256(temporaryPath, result.sha256, result.systemError)) {
        std::remove(temporaryPath.c_str());
        result.message = "Unable to verify the created ZIP archive";
        return result;
    }

    const std::string finalPath = destinationDirectory
        + "/" + timestamp + "_" + result.sha256.substr(0, 12) + ".zip";
    if (fileExists(finalPath)) {
        std::remove(temporaryPath.c_str());
    } else if (std::rename(temporaryPath.c_str(), finalPath.c_str()) != 0) {
        result.systemError = errno;
        std::remove(temporaryPath.c_str());
        result.message = "Unable to assign the final backup name";
        return result;
    }

    result.success = true;
    result.message = "Local backup completed";
    result.archivePath = finalPath;
    result.fileCount = archiveContext.progress.filesProcessed;
    result.uncompressedBytes = archiveContext.progress.bytesProcessed;
    int stateError = 0;
    const bool stateWritten = writeLocalBackupState(
            identity,
            user,
            save,
            result.archivePath,
            result.sha256,
            result.revisionId,
            result.parentRevisionId,
            result.parentRevisionIds,
            result.payloadSha256,
            timestamp,
            result.fileCount,
            result.uncompressedBytes,
            stateError);
    if (!stateWritten) {
        result.message += " (change index was not updated)";
    } else if (restoreAnchor.valid) {
        int anchorError = 0;
        result.lineageAnchorConsumed = clearRestoreLineageAnchor(
                identity,
                user,
                save.applicationId,
                anchorError);
        if (!result.lineageAnchorConsumed) {
            result.message += " (restore continuity was not removed, errno "
                + std::to_string(anchorError) + ")";
        }
    }
    return result;
}

} // namespace nxsync
