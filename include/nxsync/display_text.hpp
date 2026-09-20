#pragma once

#include <cstddef>
#include <string>

namespace nxsync {

struct DisplayText {
    std::string value;
    std::size_t visibleCodepoints{0};
};

DisplayText sanitizeDisplayUtf8(const char* value, std::size_t maximumLength);

} // namespace nxsync
