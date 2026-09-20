#include "nxsync/credential_crypto.hpp"

#include <mbedtls/gcm.h>
#include <switch.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace nxsync {
namespace {

constexpr std::size_t AesKeySize = 16;
constexpr std::size_t WrappedKeySize = 16;
constexpr std::size_t NonceSize = 12;
constexpr std::size_t AuthenticationTagSize = 16;
constexpr std::size_t MaximumCredentialSize = 256;
constexpr std::uint32_t DeviceUniqueKekOption = 1U;

// First 128 bits of SHA-256("NXSync Nextcloud credential KEK v1"). This is a
// public domain separator, not a secret. SPL combines it with the console's
// device-unique master key and never stores that master key on the SD card.
constexpr std::array<unsigned char, AesKeySize> CredentialKekSource{
    0x2d, 0xe9, 0xac, 0x0d, 0x2e, 0x2a, 0x38, 0xa9,
    0x7c, 0xa8, 0x2b, 0xca, 0x0b, 0x54, 0x6b, 0x8a};

constexpr const char* AadDomain = "NXSync credential AES-GCM AAD v1";

void secureZero(void* pointer, const std::size_t size) {
    volatile unsigned char* output =
        static_cast<volatile unsigned char*>(pointer);
    for (std::size_t index = 0; index < size; ++index) output[index] = 0;
}

std::string resultCode(const Result result) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", result);
    return buffer;
}

char hexDigit(const unsigned value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('a' + value - 10);
}

std::string encodeHex(const unsigned char* data, const std::size_t size) {
    std::string output(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        output[index * 2] = hexDigit(data[index] >> 4);
        output[index * 2 + 1] = hexDigit(data[index] & 0x0fU);
    }
    return output;
}

int decodeHexDigit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decodeHex(const std::string& encoded, std::vector<unsigned char>& output) {
    if (encoded.empty() || encoded.size() % 2 != 0) return false;
    output.resize(encoded.size() / 2);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = decodeHexDigit(encoded[index * 2]);
        const int low = decodeHexDigit(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) {
            secureZero(output.data(), output.size());
            output.clear();
            return false;
        }
        output[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

std::string encodeGeneration(const std::uint32_t generation) {
    char buffer[9]{};
    std::snprintf(buffer, sizeof(buffer), "%08x", generation);
    return buffer;
}

bool decodeGeneration(const std::string& encoded, std::uint32_t& generation) {
    if (encoded.size() != 8) return false;
    generation = 0;
    for (const char value : encoded) {
        const int digit = decodeHexDigit(value);
        if (digit < 0) return false;
        generation = (generation << 4U) | static_cast<std::uint32_t>(digit);
    }
    return true;
}

std::vector<std::string> splitProtectedValue(const std::string& value) {
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

std::string makeAad(
    const std::string& generation,
    const std::string& wrappedKey,
    const std::string& nonce) {
    return std::string(AadDomain) + "|" + generation + "|" + wrappedKey
        + "|" + nonce;
}

class SplSessions {
public:
    bool initialize(std::string& error) {
        Result result = splInitialize();
        if (R_FAILED(result)) {
            error = "Unable to initialize SPL (" + resultCode(result) + ")";
            return false;
        }
        generalInitialized_ = true;
        result = splCryptoInitialize();
        if (R_FAILED(result)) {
            error = "Unable to initialize SPL crypto (" + resultCode(result) + ")";
            return false;
        }
        cryptoInitialized_ = true;
        return true;
    }

    ~SplSessions() {
        if (cryptoInitialized_) splCryptoExit();
        if (generalInitialized_) splExit();
    }

private:
    bool generalInitialized_{false};
    bool cryptoInitialized_{false};
};

bool deriveCredentialKey(
    const std::uint32_t generation,
    const unsigned char* wrappedKey,
    std::array<unsigned char, AesKeySize>& key,
    std::string& error) {
    std::array<unsigned char, AesKeySize> sealedKek{};
    Result result = splCryptoGenerateAesKek(
        CredentialKekSource.data(),
        generation,
        DeviceUniqueKekOption,
        sealedKek.data());
    if (R_SUCCEEDED(result)) {
        result = splCryptoGenerateAesKey(
            sealedKek.data(), wrappedKey, key.data());
    }
    secureZero(sealedKek.data(), sealedKek.size());
    if (R_FAILED(result)) {
        secureZero(key.data(), key.size());
        error = "Unable to derive the console credential key ("
            + resultCode(result) + ")";
        return false;
    }
    return true;
}

} // namespace

bool protectNextcloudCredential(
    const std::string& plaintext,
    std::string& protectedValue,
    std::string& error) {
    protectedValue.clear();
    error.clear();
    if (plaintext.empty() || plaintext.size() > MaximumCredentialSize) {
        error = "Application password is missing or too long";
        return false;
    }

    SplSessions sessions;
    if (!sessions.initialize(error)) {
        return false;
    }

    u64 rawGeneration = 0;
    Result result = splGetConfig(
        SplConfigItem_NewKeyGeneration, &rawGeneration);
    if (R_FAILED(result)
        || rawGeneration > std::numeric_limits<std::uint32_t>::max()) {
        error = R_FAILED(result)
            ? "Unable to read the device key generation (" + resultCode(result) + ")"
            : "Unsupported device key generation";
        return false;
    }
    const std::uint32_t generation = static_cast<std::uint32_t>(rawGeneration);

    std::array<unsigned char, WrappedKeySize> wrappedKey{};
    std::array<unsigned char, NonceSize> nonce{};
    result = splGetRandomBytes(wrappedKey.data(), wrappedKey.size());
    if (R_SUCCEEDED(result)) {
        result = splGetRandomBytes(nonce.data(), nonce.size());
    }
    if (R_FAILED(result)) {
        secureZero(wrappedKey.data(), wrappedKey.size());
        secureZero(nonce.data(), nonce.size());
        error = "Unable to generate credential randomness ("
            + resultCode(result) + ")";
        return false;
    }

    std::array<unsigned char, AesKeySize> key{};
    if (!deriveCredentialKey(generation, wrappedKey.data(), key, error)) {
        secureZero(wrappedKey.data(), wrappedKey.size());
        secureZero(nonce.data(), nonce.size());
        return false;
    }

    const std::string generationHex = encodeGeneration(generation);
    const std::string wrappedKeyHex = encodeHex(wrappedKey.data(), wrappedKey.size());
    const std::string nonceHex = encodeHex(nonce.data(), nonce.size());
    const std::string aad = makeAad(generationHex, wrappedKeyHex, nonceHex);
    std::vector<unsigned char> ciphertext(plaintext.size());
    std::array<unsigned char, AuthenticationTagSize> tag{};

    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int cryptoResult = mbedtls_gcm_setkey(
        &context, MBEDTLS_CIPHER_ID_AES, key.data(), AesKeySize * 8);
    if (cryptoResult == 0) {
        cryptoResult = mbedtls_gcm_crypt_and_tag(
            &context,
            MBEDTLS_GCM_ENCRYPT,
            plaintext.size(),
            nonce.data(), nonce.size(),
            reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            ciphertext.data(),
            tag.size(), tag.data());
    }
    mbedtls_gcm_free(&context);
    secureZero(key.data(), key.size());
    secureZero(wrappedKey.data(), wrappedKey.size());
    secureZero(nonce.data(), nonce.size());
    if (cryptoResult != 0) {
        secureZero(ciphertext.data(), ciphertext.size());
        secureZero(tag.data(), tag.size());
        error = "Unable to encrypt the application password (mbedTLS "
            + std::to_string(cryptoResult) + ")";
        return false;
    }

    protectedValue = std::string(EncryptedCredentialPrefix)
        + generationHex + ":" + wrappedKeyHex + ":" + nonceHex + ":"
        + encodeHex(ciphertext.data(), ciphertext.size()) + ":"
        + encodeHex(tag.data(), tag.size());
    secureZero(ciphertext.data(), ciphertext.size());
    secureZero(tag.data(), tag.size());
    return true;
}

bool unprotectNextcloudCredential(
    const std::string& protectedValue,
    std::string& plaintext,
    std::string& error) {
    secureClearString(plaintext);
    error.clear();
    const std::vector<std::string> parts = splitProtectedValue(protectedValue);
    if (parts.size() != 6 || parts[0] != "v1") {
        error = "Unsupported encrypted credential format";
        return false;
    }

    std::uint32_t generation = 0;
    std::vector<unsigned char> wrappedKey;
    std::vector<unsigned char> nonce;
    std::vector<unsigned char> ciphertext;
    std::vector<unsigned char> tag;
    if (!decodeGeneration(parts[1], generation)
        || !decodeHex(parts[2], wrappedKey)
        || wrappedKey.size() != WrappedKeySize
        || !decodeHex(parts[3], nonce)
        || nonce.size() != NonceSize
        || !decodeHex(parts[4], ciphertext)
        || ciphertext.empty() || ciphertext.size() > MaximumCredentialSize
        || !decodeHex(parts[5], tag)
        || tag.size() != AuthenticationTagSize) {
        error = "Malformed encrypted credential";
        secureZero(wrappedKey.data(), wrappedKey.size());
        secureZero(nonce.data(), nonce.size());
        secureZero(ciphertext.data(), ciphertext.size());
        secureZero(tag.data(), tag.size());
        return false;
    }

    SplSessions sessions;
    if (!sessions.initialize(error)) {
        secureZero(wrappedKey.data(), wrappedKey.size());
        secureZero(nonce.data(), nonce.size());
        secureZero(ciphertext.data(), ciphertext.size());
        secureZero(tag.data(), tag.size());
        return false;
    }
    std::array<unsigned char, AesKeySize> key{};
    if (!deriveCredentialKey(generation, wrappedKey.data(), key, error)) {
        secureZero(wrappedKey.data(), wrappedKey.size());
        secureZero(nonce.data(), nonce.size());
        secureZero(ciphertext.data(), ciphertext.size());
        secureZero(tag.data(), tag.size());
        return false;
    }

    const std::string aad = makeAad(parts[1], parts[2], parts[3]);
    plaintext.resize(ciphertext.size());
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int cryptoResult = mbedtls_gcm_setkey(
        &context, MBEDTLS_CIPHER_ID_AES, key.data(), AesKeySize * 8);
    if (cryptoResult == 0) {
        cryptoResult = mbedtls_gcm_auth_decrypt(
            &context,
            ciphertext.size(),
            nonce.data(), nonce.size(),
            reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
            tag.data(), tag.size(),
            ciphertext.data(),
            reinterpret_cast<unsigned char*>(&plaintext[0]));
    }
    mbedtls_gcm_free(&context);
    secureZero(key.data(), key.size());
    secureZero(wrappedKey.data(), wrappedKey.size());
    secureZero(nonce.data(), nonce.size());
    secureZero(ciphertext.data(), ciphertext.size());
    secureZero(tag.data(), tag.size());
    if (cryptoResult != 0) {
        secureClearString(plaintext);
        error = "The encrypted credential was modified or belongs to another console";
        return false;
    }
    return true;
}

void secureClearString(std::string& value) {
    if (!value.empty()) secureZero(&value[0], value.size());
    value.clear();
}

} // namespace nxsync
