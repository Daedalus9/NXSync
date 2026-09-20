#include "nxsync/save_catalog.hpp"

#include <cstdio>

namespace nxsync {

std::string formatTitleId(const std::uint64_t applicationId) {
    char buffer[17]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llX",
        static_cast<unsigned long long>(applicationId));
    return buffer;
}

std::string formatByteSize(const std::uint64_t bytes) {
    constexpr double KibiByte = 1024.0;
    constexpr double MebiByte = KibiByte * 1024.0;
    constexpr double GibiByte = MebiByte * 1024.0;
    char buffer[32]{};
    if (bytes >= static_cast<std::uint64_t>(GibiByte)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB", bytes / GibiByte);
    } else if (bytes >= static_cast<std::uint64_t>(MebiByte)) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB", bytes / MebiByte);
    } else if (bytes >= static_cast<std::uint64_t>(KibiByte)) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB", bytes / KibiByte);
    } else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu B",
            static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::string formatResult(const Result result) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", R_VALUE(result));
    return buffer;
}

} // namespace nxsync
