#include "nxsync/backup_state.hpp"
#include "nxsync/atomic_file.hpp"
#include <sstream>

#include "nxsync/remote_layout.hpp"
#include "nxsync/revision_parents.hpp"

#include <minizip/unzip.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace nxsync {
namespace {

constexpr const char* StateRoot = "sdmc:/config/NXSync/state";

std::string formatHex64(const std::uint64_t value) {
    char buffer[17]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llX",
        static_cast<unsigned long long>(value));
    return buffer;
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

std::string stateDirectory(
    const DeviceIdentity& identity,
    const UserSaves& user) {
    std::string device = sanitizePathSegment(identity.folderName, false);
    if (device.empty()) {
        device = "NS-UNKNOWN";
    }
    return std::string(StateRoot) + "/" + device + "/" + formatUid(user.uid);
}

std::string statePath(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save) {
    return stateDirectory(identity, user)
        + "/" + formatHex64(save.applicationId)
        + "-" + formatHex64(save.saveDataId)
        + ".ini";
}

std::string restoreAnchorPath(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const std::uint64_t applicationId) {
    return stateDirectory(identity, user)
        + "/restore-anchor-" + formatHex64(applicationId) + ".ini";
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
        if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
            systemError = errno;
            return false;
        }
    }
    return true;
}

bool parseUnsigned(const std::string& value, const int base, std::uint64_t& output) {
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, base);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return false;
    }
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool readArchiveSize(const std::string& path, std::uint64_t& size) {
    struct stat fileStat{};
    if (stat(path.c_str(), &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) {
        return false;
    }
    size = static_cast<std::uint64_t>(fileStat.st_size);
    return true;
}

std::string joinRevisionParents(const std::vector<std::string>& parents) {
    std::string result;
    for (const std::string& parent : parents) {
        if (!result.empty()) result.push_back(',');
        result += parent;
    }
    return result;
}

std::vector<std::string> splitRevisionParents(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(',', start);
        const std::string parent = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!parent.empty()) result.push_back(parent);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

enum class ArchiveContentState {
    Invalid,
    Empty,
    HasSaveFiles,
};

ArchiveContentState inspectArchiveContent(const std::string& path) {
    unzFile archive = unzOpen64(path.c_str());
    if (archive == nullptr) {
        return ArchiveContentState::Invalid;
    }

    int navigationResult = unzGoToFirstFile(archive);
    while (navigationResult == UNZ_OK) {
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
                0) != UNZ_OK
            || info.size_filename >= nameBuffer.size()) {
            navigationResult = UNZ_PARAMERROR;
            break;
        }

        const std::string name(nameBuffer.data());
        if (name.size() > 5
            && name.compare(0, 5, "save/") == 0
            && name.back() != '/') {
            unzClose(archive);
            return ArchiveContentState::HasSaveFiles;
        }
        navigationResult = unzGoToNextFile(archive);
    }

    unzClose(archive);
    return navigationResult == UNZ_END_OF_LIST_OF_FILE
        ? ArchiveContentState::Empty
        : ArchiveContentState::Invalid;
}

bool writeStateFile(
    const std::string& targetPath,
    const SaveEntry& save,
    const LocalBackupState& state,
    int& systemError) {
    std::ostringstream output;
        output
            << "version=3\n"
            << "commit_id=" << formatHex64(save.commitId) << "\n"
            << "save_data_id=" << formatHex64(save.saveDataId) << "\n"
            << "empty_save=" << (state.emptySave ? 1 : 0) << "\n"
            << "archive_size=" << state.archiveSize << "\n"
            << "archive_path=" << state.archivePath << "\n"
            << "sha256=" << state.sha256 << "\n"
            << "revision_id=" << state.revisionId << "\n"
            << "parent_revision_id=" << state.parentRevisionId << "\n"
            << "parent_revision_ids="
                << joinRevisionParents(normalizedRevisionParents(
                    state.parentRevisionId, state.parentRevisionIds)) << "\n"
            << "payload_sha256=" << state.payloadSha256 << "\n"
            << "created_utc=" << state.createdUtc << "\n"
            << "file_count=" << state.fileCount << "\n"
            << "uncompressed_bytes=" << state.uncompressedBytes << "\n"
            << "remote_uploaded=" << (state.remoteUploaded ? 1 : 0) << "\n"
            << "remote_path=" << state.remotePath << "\n";
    return writeTextFileAtomic(targetPath, output.str(), systemError);
}

} // namespace

LocalBackupState findCurrentLocalBackup(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save) {
    LocalBackupState state;
    if (!save.extraDataAvailable) {
        return state;
    }

    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(statePath(identity, user, save), text, readError)) return state;
    std::istringstream input(text);
    state.indexed = true;

    std::uint64_t version = 0;
    std::uint64_t storedCommitId = 0;
    std::uint64_t storedSaveDataId = 0;
    std::uint64_t storedArchiveSize = 0;
    std::uint64_t storedEmptySave = 0;
    std::uint64_t remoteUploaded = 0;
    std::uint64_t fileCount = 0;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "version") {
            parseUnsigned(value, 10, version);
        } else if (key == "commit_id") {
            parseUnsigned(value, 16, storedCommitId);
        } else if (key == "save_data_id") {
            parseUnsigned(value, 16, storedSaveDataId);
        } else if (key == "archive_size") {
            parseUnsigned(value, 10, storedArchiveSize);
        } else if (key == "empty_save") {
            parseUnsigned(value, 10, storedEmptySave);
        } else if (key == "archive_path") {
            state.archivePath = value;
        } else if (key == "sha256") {
            state.sha256 = value;
        } else if (key == "revision_id") {
            state.revisionId = value;
        } else if (key == "parent_revision_id") {
            state.parentRevisionId = value;
        } else if (key == "parent_revision_ids") {
            state.parentRevisionIds = splitRevisionParents(value);
        } else if (key == "payload_sha256") {
            state.payloadSha256 = value;
        } else if (key == "created_utc") {
            state.createdUtc = value;
        } else if (key == "file_count") {
            parseUnsigned(value, 10, fileCount);
        } else if (key == "uncompressed_bytes") {
            parseUnsigned(value, 10, state.uncompressedBytes);
        } else if (key == "remote_uploaded") {
            parseUnsigned(value, 10, remoteUploaded);
        } else if (key == "remote_path") {
            state.remotePath = value;
        }
    }

    state.parentRevisionIds = normalizedRevisionParents(
        state.parentRevisionId, state.parentRevisionIds);
    state.parentRevisionId = state.parentRevisionIds.empty()
        ? std::string()
        : state.parentRevisionIds.front();
    state.legacy = version > 0 && version != 3;
    state.fileCount = static_cast<std::size_t>(fileCount);
    const bool commonRecordValid = version == 3
        && storedSaveDataId == save.saveDataId;
    if (commonRecordValid && storedEmptySave == 1) {
        state.emptySave = true;
        state.saveChanged = storedCommitId != save.commitId;
        return state;
    }

    std::uint64_t actualArchiveSize = 0;
    if (!readArchiveSize(state.archivePath, actualArchiveSize)) {
        state.current = false;
        return state;
    }
    state.archivePresent = true;
    state.archiveSize = actualArchiveSize;
    const bool archiveRecordValid = commonRecordValid
        && storedArchiveSize == actualArchiveSize
        && state.sha256.size() == 64
        && state.revisionId.size() == 64
        && validRevisionParents(
            state.revisionId,
            state.parentRevisionId,
            state.parentRevisionIds)
        && state.payloadSha256.size() == 64
        && !state.archivePath.empty();
    const ArchiveContentState contentState = archiveRecordValid
        ? inspectArchiveContent(state.archivePath)
        : ArchiveContentState::Invalid;
    state.emptySave = archiveRecordValid
        && contentState == ArchiveContentState::Empty;
    state.recordValid = archiveRecordValid
        && contentState == ArchiveContentState::HasSaveFiles;
    state.saveChanged = (state.recordValid || state.emptySave)
        && storedCommitId != save.commitId;
    state.current = state.recordValid && !state.saveChanged;
    state.remoteUploaded = state.current && remoteUploaded == 1 && !state.remotePath.empty();
    return state;
}

bool writeLocalEmptySaveState(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    int& systemError) {
    if (!save.extraDataAvailable) {
        return true;
    }
    const std::string directory = stateDirectory(identity, user);
    if (!createDirectories(directory, systemError)) {
        return false;
    }
    LocalBackupState state;
    state.indexed = true;
    state.emptySave = true;
    return writeStateFile(statePath(identity, user, save), save, state, systemError);
}

bool writeLocalBackupState(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    const std::string& archivePath,
    const std::string& sha256,
    const std::string& revisionId,
    const std::string& parentRevisionId,
    const std::vector<std::string>& parentRevisionIds,
    const std::string& payloadSha256,
    const std::string& createdUtc,
    const std::size_t fileCount,
    const std::uint64_t uncompressedBytes,
    int& systemError) {
    if (!save.extraDataAvailable) {
        return true;
    }

    std::uint64_t archiveSize = 0;
    if (!readArchiveSize(archivePath, archiveSize)) {
        systemError = errno;
        return false;
    }

    const std::string directory = stateDirectory(identity, user);
    if (!createDirectories(directory, systemError)) {
        return false;
    }

    LocalBackupState state;
    state.indexed = true;
    state.archivePresent = true;
    state.recordValid = true;
    state.current = true;
    state.archivePath = archivePath;
    state.sha256 = sha256;
    state.revisionId = revisionId;
    state.parentRevisionId = parentRevisionId;
    state.parentRevisionIds = normalizedRevisionParents(
        parentRevisionId, parentRevisionIds);
    state.payloadSha256 = payloadSha256;
    state.createdUtc = createdUtc;
    state.fileCount = fileCount;
    state.uncompressedBytes = uncompressedBytes;
    state.archiveSize = archiveSize;
    return writeStateFile(statePath(identity, user, save), save, state, systemError);
}

bool markLocalBackupUploaded(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const SaveEntry& save,
    const std::string& remotePath,
    int& systemError) {
    LocalBackupState state = findCurrentLocalBackup(identity, user, save);
    if (!state.current || remotePath.empty()) {
        systemError = EINVAL;
        return false;
    }
    state.remoteUploaded = true;
    state.remotePath = remotePath;
    return writeStateFile(statePath(identity, user, save), save, state, systemError);
}

RestoreLineageAnchor loadRestoreLineageAnchor(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const std::uint64_t applicationId) {
    RestoreLineageAnchor anchor;
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(restoreAnchorPath(identity, user, applicationId), text, readError)) return anchor;
    std::istringstream input(text);

    std::uint64_t version = 0;
    std::uint64_t storedTitleId = 0;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "version") parseUnsigned(value, 10, version);
        else if (key == "title_id") parseUnsigned(value, 16, storedTitleId);
        else if (key == "revision_id") anchor.revisionId = value;
        else if (key == "payload_sha256") anchor.payloadSha256 = value;
        else if (key == "parent_revision_ids") {
            anchor.parentRevisionIds = splitRevisionParents(value);
        }
    }
    anchor.parentRevisionIds = normalizedRevisionParents(
        anchor.revisionId, anchor.parentRevisionIds);
    anchor.valid = version == 1
        && storedTitleId == applicationId
        && validateRestoreLineageAnchor(RestoreLineageAnchor{
            true,
            anchor.revisionId,
            anchor.payloadSha256,
            anchor.parentRevisionIds});
    return anchor;
}

bool writeRestoreLineageAnchor(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const std::uint64_t applicationId,
    const std::string& revisionId,
    const std::string& payloadSha256,
    const std::vector<std::string>& parentRevisionIds,
    int& systemError) {
    const RestoreLineageAnchor anchor{
        true,
        revisionId,
        payloadSha256,
        normalizedRevisionParents(revisionId, parentRevisionIds)};
    if (!validateRestoreLineageAnchor(anchor)) {
        systemError = EINVAL;
        return false;
    }
    const std::string directory = stateDirectory(identity, user);
    if (!createDirectories(directory, systemError)) return false;
    const std::string target = restoreAnchorPath(identity, user, applicationId);
    std::ostringstream output;
        output << "version=1\n"
            << "title_id=" << formatHex64(applicationId) << "\n"
            << "revision_id=" << revisionId << "\n"
            << "payload_sha256=" << payloadSha256 << "\n"
            << "parent_revision_ids="
                << joinRevisionParents(anchor.parentRevisionIds) << "\n";
    return writeTextFileAtomic(target, output.str(), systemError);
}

bool clearRestoreLineageAnchor(
    const DeviceIdentity& identity,
    const UserSaves& user,
    const std::uint64_t applicationId,
    int& systemError) {
    const std::string target = restoreAnchorPath(identity, user, applicationId);
    return removeTextFileRecoverable(target, systemError);
}

} // namespace nxsync
