#include "nxsync/credential_crypto.hpp"
#include <switch.h>
#include <cassert>
#include <string>
int main() {
    std::string cipher, plain, error, other;
    const std::string secret = "test-only application credential";
    assert(nxsync::protectNextcloudCredential(secret, cipher, error));
    assert(nxsync::unprotectNextcloudCredential(cipher, plain, error) && plain == secret);
    assert(nxsync::protectNextcloudCredential(secret, other, error) && other != cipher);
    for (std::size_t i = 3; i < cipher.size(); ++i) {
        if (cipher[i] == ':') continue;
        auto corrupt = cipher;
        corrupt[i] = corrupt[i] == 'a' ? 'b' : 'a';
        plain = "must be cleared";
        assert(!nxsync::unprotectNextcloudCredential(corrupt, plain, error));
        assert(plain.empty());
    }
    ++testDeviceSecret;
    assert(!nxsync::unprotectNextcloudCredential(cipher, plain, error) && plain.empty());
    --testDeviceSecret;
    testSplFailure = true;
    assert(!nxsync::protectNextcloudCredential(secret, other, error) && other.empty());
    testSplFailure = false;
    assert(!nxsync::protectNextcloudCredential("", other, error));
    assert(!nxsync::protectNextcloudCredential(std::string(257, 'x'), other, error));
    assert(nxsync::protectNextcloudCredential(std::string(256, 'x'), other, error));
    assert(nxsync::unprotectNextcloudCredential(other, plain, error) && plain.size() == 256);
    assert(!nxsync::unprotectNextcloudCredential(cipher.substr(0, cipher.size()-1), plain, error));
}
