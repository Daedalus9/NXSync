#include "nxsync/credential_crypto.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nxsync {
namespace {

std::string hexEncode(const std::string& value) {
    constexpr char Hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() * 2);
    for (const unsigned char ch : value) {
        const unsigned char encoded = ch ^ 0xa5U;
        output.push_back(Hex[encoded >> 4]);
        output.push_back(Hex[encoded & 0x0fU]);
    }
    return output;
}

int digit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool hexDecode(const std::string& encoded, std::string& output) {
    if (encoded.empty() || encoded.size() % 2 != 0) return false;
    output.resize(encoded.size() / 2);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = digit(encoded[index * 2]);
        const int low = digit(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<char>(((high << 4) | low) ^ 0xa5U);
    }
    return true;
}

std::string testTag(const std::string& ciphertext) {
    std::uint64_t first = 1469598103934665603ULL;
    std::uint64_t second = 1099511628211ULL;
    for (const unsigned char ch : ciphertext) {
        first = (first ^ ch) * 1099511628211ULL;
        second = (second + ch) * 0x9e3779b185ebca87ULL;
    }
    char output[33]{};
    std::snprintf(
        output, sizeof(output), "%016llx%016llx",
        static_cast<unsigned long long>(first),
        static_cast<unsigned long long>(second));
    return output;
}

std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(':', start);
        if (separator == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, separator - start));
        start = separator + 1;
    }
    return parts;
}

} // namespace

bool protectNextcloudCredential(
    const std::string& plaintext,
    std::string& protectedValue,
    std::string& error) {
    error.clear();
    if (plaintext.empty()) {
        error = "Missing test credential";
        return false;
    }
    const std::string ciphertext = hexEncode(plaintext);
    protectedValue = std::string(EncryptedCredentialPrefix)
        + "00000000:00112233445566778899aabbccddeeff:"
        + "00112233445566778899aabb:" + ciphertext + ":"
        + testTag(ciphertext);
    return true;
}

bool unprotectNextcloudCredential(
    const std::string& protectedValue,
    std::string& plaintext,
    std::string& error) {
    error.clear();
    const std::vector<std::string> parts = split(protectedValue);
    if (parts.size() != 6 || parts[0] != "v1"
        || parts[5] != testTag(parts.size() > 4 ? parts[4] : std::string())
        || !hexDecode(parts[4], plaintext)) {
        secureClearString(plaintext);
        error = "Test credential authentication failed";
        return false;
    }
    return true;
}

void secureClearString(std::string& value) {
    volatile char* data = value.empty() ? nullptr : &value[0];
    for (std::size_t index = 0; index < value.size(); ++index) data[index] = 0;
    value.clear();
}

} // namespace nxsync
