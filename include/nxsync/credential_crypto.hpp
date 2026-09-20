#pragma once

#include <string>

namespace nxsync {

constexpr const char* EncryptedCredentialPrefix = "v1:";

bool protectNextcloudCredential(
    const std::string& plaintext,
    std::string& protectedValue,
    std::string& error);

bool unprotectNextcloudCredential(
    const std::string& protectedValue,
    std::string& plaintext,
    std::string& error);

void secureClearString(std::string& value);

} // namespace nxsync
