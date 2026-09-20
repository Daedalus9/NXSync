#include "nxsync/display_text.hpp"

#include <cstdint>

namespace nxsync {
namespace {

bool excludedCodepoint(const std::uint32_t codepoint) {
    return codepoint == 0x7F
        || (codepoint >= 0x80 && codepoint <= 0x9F)
        || codepoint == 0x200B
        || codepoint == 0x200C
        || codepoint == 0x200D
        || codepoint == 0x2060
        || codepoint == 0xFEFF
        || codepoint == 0xFFFD
        || (codepoint >= 0xD800 && codepoint <= 0xDFFF)
        || (codepoint >= 0xE000 && codepoint <= 0xF8FF)
        || (codepoint >= 0xF0000 && codepoint <= 0xFFFFD)
        || (codepoint >= 0x100000 && codepoint <= 0x10FFFD)
        || (codepoint & 0xFFFFU) == 0xFFFEU
        || (codepoint & 0xFFFFU) == 0xFFFFU;
}

} // namespace

DisplayText sanitizeDisplayUtf8(const char* value, const std::size_t maximumLength) {
    DisplayText result;
    if (value == nullptr) {
        return result;
    }

    result.value.reserve(maximumLength);
    std::size_t index = 0;
    bool previousSpace = false;
    while (index < maximumLength && value[index] != '\0') {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if (first == '\n' || first == '\r' || first == '\t' || first == ' ') {
            if (!result.value.empty() && !previousSpace) {
                result.value.push_back(' ');
                previousSpace = true;
            }
            ++index;
            continue;
        }
        if (first < 0x20) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first < 0x80) {
            length = 1;
            codepoint = first;
        } else if ((first & 0xE0U) == 0xC0U) {
            length = 2;
            codepoint = first & 0x1FU;
            minimum = 0x80;
        } else if ((first & 0xF0U) == 0xE0U) {
            length = 3;
            codepoint = first & 0x0FU;
            minimum = 0x800;
        } else if ((first & 0xF8U) == 0xF0U) {
            length = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000;
        } else {
            ++index;
            continue;
        }
        if (index + length > maximumLength) {
            break;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char continuation =
                static_cast<unsigned char>(value[index + offset]);
            if (value[index + offset] == '\0' || (continuation & 0xC0U) != 0x80U) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        if (!valid || (length > 1 && codepoint < minimum)
            || codepoint > 0x10FFFF || excludedCodepoint(codepoint)) {
            ++index;
            continue;
        }

        result.value.append(value + index, length);
        ++result.visibleCodepoints;
        previousSpace = false;
        index += length;
    }
    while (!result.value.empty() && result.value.back() == ' ') {
        result.value.pop_back();
    }
    return result;
}

} // namespace nxsync
