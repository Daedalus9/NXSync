#pragma once
// Host stand-ins only for hardware SPL/session calls. AES-GCM is the real
// mbedTLS implementation compiled through source/credential_crypto.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>
using Result = std::uint32_t;
using u64 = std::uint64_t;
#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
inline unsigned char testDeviceSecret = 0x5a;
inline bool testSplFailure = false;
inline Result splInitialize() { return testSplFailure ? 1 : 0; }
inline Result splCryptoInitialize() { return 0; }
inline void splExit() {}
inline void splCryptoExit() {}
constexpr int SplConfigItem_NewKeyGeneration = 1;
inline Result splGetConfig(int, u64* value) { *value = 7; return 0; }
inline Result splCryptoGenerateAesKek(const void* source, unsigned, unsigned, void* out) {
    const auto* input = static_cast<const unsigned char*>(source);
    auto* output = static_cast<unsigned char*>(out);
    for (int i = 0; i < 16; ++i) output[i] = input[i] ^ testDeviceSecret;
    return 0;
}
inline Result splCryptoGenerateAesKey(const void* kek, const void* wrapped, void* key) {
    for (int i = 0; i < 16; ++i) static_cast<unsigned char*>(key)[i] =
        static_cast<const unsigned char*>(kek)[i] ^ static_cast<const unsigned char*>(wrapped)[i];
    return 0;
}
inline Result splGetRandomBytes(void* output, std::size_t count) {
    static unsigned char next = 1;
    for (std::size_t i = 0; i < count; ++i) static_cast<unsigned char*>(output)[i] = next++;
    return 0;
}
