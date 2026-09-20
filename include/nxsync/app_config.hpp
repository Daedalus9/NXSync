#pragma once

#include <cstddef>
#include <string>

namespace nxsync {

struct AppConfig {
    std::string remoteRoot{"NXSync"};
    std::string deviceIdOverride;
    std::string nextcloudUrl;
    std::string nextcloudUsername;
    std::string nextcloudAppPassword;
    bool credentialEncrypted{false};
    bool credentialMigrated{false};
    std::string credentialError;
    bool autoBackupOnStart{false};
    std::size_t retentionCount{5};

    bool nextcloudConfigured() const {
        return !nextcloudUrl.empty()
            && !nextcloudUsername.empty()
            && !nextcloudAppPassword.empty()
            && credentialError.empty();
    }
};

AppConfig loadOrCreateConfig(const std::string& path);
bool validateAppConfig(const AppConfig& config, std::string& error);
bool saveAppConfig(
    const std::string& path,
    AppConfig& config,
    std::string& error);

} // namespace nxsync
