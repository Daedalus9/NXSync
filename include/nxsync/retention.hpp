#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nxsync {

struct RetentionResult {
    std::size_t removed{0};
    std::size_t failed{0};
    int systemError{0};
    std::string firstError;
};

bool isNxsyncBackupFilename(const std::string& filename);

std::vector<std::string> selectBackupsToPrune(
    const std::vector<std::string>& filenames,
    std::size_t keepCount,
    const std::string& protectedFilename);

RetentionResult pruneLocalBackups(
    const std::string& directory,
    const std::string& protectedFilename,
    std::size_t keepCount);

} // namespace nxsync
