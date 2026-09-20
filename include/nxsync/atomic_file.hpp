#pragma once

#include <cstddef>
#include <string>

namespace nxsync {

// A path has one owning writer (protocol owner / serialized queue consumer).
// Flushes the complete new generation before publication. Filesystems without
// replace-on-rename retain the previous generation as .bak until publication.
// Readers use readTextFileRecoverable to survive an interrupted rename.
bool writeTextFileAtomic(
    const std::string& path,
    const std::string& text,
    int& systemError);

bool readTextFileRecoverable(
    const std::string& path,
    std::string& text,
    int& systemError,
    std::size_t maximumBytes = 64 * 1024);

bool removeTextFileRecoverable(const std::string& path, int& systemError);

} // namespace nxsync
