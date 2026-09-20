#include "nxsync/retention.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <vector>

namespace nxsync {
namespace {

bool isDigit(const char ch) {
    return ch >= '0' && ch <= '9';
}

bool isHex(const char ch) {
    return isDigit(ch)
        || (ch >= 'a' && ch <= 'f')
        || (ch >= 'A' && ch <= 'F');
}

} // namespace

bool isNxsyncBackupFilename(const std::string& filename) {
    // YYYY-MM-DDTHHMMSSZ_0123456789ab.zip
    if (filename.size() != 35
        || filename[4] != '-'
        || filename[7] != '-'
        || filename[10] != 'T'
        || filename[17] != 'Z'
        || filename[18] != '_'
        || filename.compare(31, 4, ".zip") != 0) {
        return false;
    }
    for (std::size_t index = 0; index < 18; ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 17) {
            continue;
        }
        if (!isDigit(filename[index])) {
            return false;
        }
    }
    for (std::size_t index = 19; index < 31; ++index) {
        if (!isHex(filename[index])) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> selectBackupsToPrune(
    const std::vector<std::string>& filenames,
    const std::size_t keepCount,
    const std::string& protectedFilename) {
    if (keepCount == 0) {
        return {};
    }

    std::vector<std::string> candidates;
    for (const std::string& filename : filenames) {
        if (isNxsyncBackupFilename(filename)
            && std::find(candidates.begin(), candidates.end(), filename) == candidates.end()) {
            candidates.push_back(filename);
        }
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<std::string>());

    const auto protectedEntry = std::find(
        candidates.begin(),
        candidates.end(),
        protectedFilename);
    if (protectedEntry != candidates.end() && protectedEntry != candidates.begin()) {
        const std::string current = *protectedEntry;
        candidates.erase(protectedEntry);
        candidates.insert(candidates.begin(), current);
    }
    if (candidates.size() <= keepCount) {
        return {};
    }
    return std::vector<std::string>(
        candidates.begin() + static_cast<std::ptrdiff_t>(keepCount),
        candidates.end());
}

RetentionResult pruneLocalBackups(
    const std::string& directory,
    const std::string& protectedFilename,
    const std::size_t keepCount) {
    RetentionResult result;
    if (keepCount == 0) {
        return result;
    }
    DIR* handle = opendir(directory.c_str());
    if (handle == nullptr) {
        result.failed = 1;
        result.systemError = errno;
        result.firstError = "Unable to read the local backup folder";
        return result;
    }

    std::vector<std::string> filenames;
    errno = 0;
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            filenames.push_back(name);
        }
    }
    const int readError = errno;
    closedir(handle);
    if (readError != 0) {
        result.failed = 1;
        result.systemError = readError;
        result.firstError = "Error while listing local backups";
        return result;
    }

    for (const std::string& filename : selectBackupsToPrune(
             filenames,
             keepCount,
             protectedFilename)) {
        const std::string path = directory
            + (directory.empty() || directory.back() == '/' ? "" : "/")
            + filename;
        if (std::remove(path.c_str()) == 0) {
            ++result.removed;
        } else {
            ++result.failed;
            if (result.firstError.empty()) {
                result.systemError = errno;
                result.firstError = "Unable to delete " + filename;
            }
        }
    }
    return result;
}

} // namespace nxsync
