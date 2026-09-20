#include "nxsync/app_config.hpp"

#include "nxsync/credential_crypto.hpp"
#include "nxsync/nextcloud_paths.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace nxsync {
namespace {

std::string trim(std::string value) {
    const auto isNotSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(), value.end());
    return value;
}

bool parseBoolean(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::size_t parseRetentionCount(const std::string& value) {
    if (value.empty()) {
        return 5;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    return errno == 0 && end != value.c_str() && *end == '\0' && parsed <= 100
        ? static_cast<std::size_t>(parsed)
        : 5;
}

void ensureConfigDirectory() {
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/NXSync", 0777);
}

// Best-effort removal for a legacy plaintext backup. Flash translation layers
// cannot provide a forensic secure-delete guarantee, but overwriting the
// logical file prevents the old credential from remaining in a visible .bak.
void scrubAndRemoveFile(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "r+b");
    if (file != nullptr) {
        if (std::fseek(file, 0, SEEK_END) == 0) {
            const long length = std::ftell(file);
            if (length > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
                unsigned char zeros[1024]{};
                long remaining = length;
                while (remaining > 0) {
                    const std::size_t amount = static_cast<std::size_t>(
                        std::min<long>(remaining, sizeof(zeros)));
                    if (std::fwrite(zeros, 1, amount, file) != amount) break;
                    remaining -= static_cast<long>(amount);
                }
                std::fflush(file);
            }
        }
        std::fclose(file);
    }
    std::remove(path.c_str());
}

void writeDefaultConfig(const std::string& path, const AppConfig& config) {
    ensureConfigDirectory();
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return;
    }

    output
        << "# NXSync configuration\n"
        << "# Remote layout: <remote_root>/<device-id>/<profile>/<title-id>/<backup>\n"
        << "remote_root=" << config.remoteRoot << "\n"
        << "# Optional. Example: NS-OLED-LIVINGROOM\n"
        << "device_id_override=\n"
        << "# Nextcloud instance root or complete personal WebDAV URL. HTTPS only.\n"
        << "nextcloud_url=\n"
        << "nextcloud_username=\n"
        << "# Encrypted with a key bound to this console. Use a revocable app password.\n"
        << "nextcloud_app_password_encrypted=\n"
        << "# Run incremental backup and pending uploads when NXSync starts.\n"
        << "auto_backup_on_start=false\n"
        << "# Backups kept per console/profile/game; default: 5.\n"
        << "retention_count=" << config.retentionCount << "\n";
}

bool containsInvalidConfigCharacter(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch == '\n' || ch == '\r' || ch == '\0';
    });
}

bool writeConfigFile(
    const std::string& path,
    const AppConfig& config,
    const std::string& protectedCredential) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return false;
    }
    output
        << "# NXSync configuration\n"
        << "# Remote layout: <remote_root>/<device-id>/<profile>/<title-id>/<backup>\n"
        << "remote_root=" << config.remoteRoot << "\n"
        << "# Optional console identifier override.\n"
        << "device_id_override=" << config.deviceIdOverride << "\n"
        << "# Nextcloud instance root or complete personal WebDAV URL. HTTPS only.\n"
        << "nextcloud_url=" << config.nextcloudUrl << "\n"
        << "nextcloud_username=" << config.nextcloudUsername << "\n"
        << "# AES-GCM credential bound to this console; do not copy it to another console.\n"
        << "nextcloud_app_password_encrypted=" << protectedCredential << "\n"
        << "# Run incremental backup and pending uploads when NXSync starts.\n"
        << "auto_backup_on_start="
        << (config.autoBackupOnStart ? "true" : "false") << "\n"
        << "# Backups kept per console/profile/game; default: 5.\n"
        << "retention_count=" << config.retentionCount << "\n";
    output.flush();
    return output.good();
}

void appendMissingConfigKeys(
    const std::string& path,
    const bool hasUrl,
    const bool hasUsername,
    const bool hasAppPassword,
    const bool hasAutoBackupOnStart,
    const bool hasRetentionCount) {
    if (hasUrl && hasUsername && hasAppPassword
        && hasAutoBackupOnStart && hasRetentionCount) {
        return;
    }
    std::ofstream output(path, std::ios::app);
    if (!output) {
        return;
    }
    if (!hasUrl || !hasUsername || !hasAppPassword) {
        output << "\n# Nextcloud WebDAV (HTTPS and application password recommended)\n";
        if (!hasUrl) {
            output << "nextcloud_url=\n";
        }
        if (!hasUsername) {
            output << "nextcloud_username=\n";
        }
        if (!hasAppPassword) {
            output << "nextcloud_app_password_encrypted=\n";
        }
    }
    if (!hasAutoBackupOnStart) {
        output << "\n# Incremental backup and pending uploads at application startup.\n"
            << "auto_backup_on_start=false\n";
    }
    if (!hasRetentionCount) {
        output << "\n# Backups kept per console/profile/game; default: 5.\n"
            << "retention_count=5\n";
    }
}

} // namespace

AppConfig loadOrCreateConfig(const std::string& path) {
    AppConfig config;
    std::ifstream input(path);
    if (!input) {
        const std::string backupPath = path + ".bak";
        std::ifstream backupInput(backupPath);
        if (backupInput) {
            backupInput.close();
            if (std::rename(backupPath.c_str(), path.c_str()) == 0) {
                input.open(path);
            }
        }
    }
    if (!input) {
        writeDefaultConfig(path, config);
        return config;
    }

    bool hasNextcloudUrl = false;
    bool hasNextcloudUsername = false;
    bool hasEncryptedAppPassword = false;
    bool hasLegacyAppPassword = false;
    bool hasAutoBackupOnStart = false;
    bool hasRetentionCount = false;
    std::string protectedCredential;
    std::string legacyCredential;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key == "remote_root" && !value.empty()) {
            config.remoteRoot = value;
        } else if (key == "device_id_override") {
            config.deviceIdOverride = value;
        } else if (key == "nextcloud_url") {
            hasNextcloudUrl = true;
            config.nextcloudUrl = value;
        } else if (key == "nextcloud_username") {
            hasNextcloudUsername = true;
            config.nextcloudUsername = value;
        } else if (key == "nextcloud_app_password_encrypted") {
            hasEncryptedAppPassword = true;
            protectedCredential = value;
        } else if (key == "nextcloud_app_password") {
            hasLegacyAppPassword = true;
            legacyCredential = value;
        } else if (key == "auto_backup_on_start") {
            hasAutoBackupOnStart = true;
            config.autoBackupOnStart = parseBoolean(value);
        } else if (key == "retention_count") {
            hasRetentionCount = true;
            config.retentionCount = parseRetentionCount(value);
        }
    }

    input.close();
    appendMissingConfigKeys(
        path,
        hasNextcloudUrl,
        hasNextcloudUsername,
        hasEncryptedAppPassword || hasLegacyAppPassword,
        hasAutoBackupOnStart,
        hasRetentionCount);

    if (!protectedCredential.empty()) {
        config.credentialEncrypted = true;
        if (!unprotectNextcloudCredential(
                protectedCredential,
                config.nextcloudAppPassword,
                config.credentialError)) {
            secureClearString(config.nextcloudAppPassword);
        }
    } else if (!legacyCredential.empty()) {
        config.nextcloudAppPassword = legacyCredential;
        std::string migrationError;
        if (saveAppConfig(path, config, migrationError)) {
            config.credentialMigrated = true;
        } else {
            config.credentialError = "Plaintext credential migration failed: "
                + migrationError;
            secureClearString(config.nextcloudAppPassword);
        }
    }
    secureClearString(legacyCredential);

    return config;
}

bool validateAppConfig(const AppConfig& config, std::string& error) {
    error.clear();
    if (!config.credentialError.empty()) {
        error = config.credentialError;
        return false;
    }
    if (config.remoteRoot.empty() || config.remoteRoot.size() > 128) {
        error = "Invalid remote folder";
        return false;
    }
    if (config.nextcloudUrl.size() > 512 || !isValidNextcloudUrl(config.nextcloudUrl)) {
        error = "Invalid Nextcloud URL: use https:// without spaces";
        return false;
    }
    if (config.nextcloudUsername.empty() || config.nextcloudUsername.size() > 128) {
        error = "Invalid Nextcloud username";
        return false;
    }
    if (config.nextcloudAppPassword.empty()
        || config.nextcloudAppPassword.size() > 256) {
        error = "Application password is missing or too long";
        return false;
    }
    if (containsInvalidConfigCharacter(config.remoteRoot)
        || containsInvalidConfigCharacter(config.deviceIdOverride)
        || containsInvalidConfigCharacter(config.nextcloudUsername)
        || containsInvalidConfigCharacter(config.nextcloudAppPassword)) {
        error = "The configuration contains invalid characters";
        return false;
    }
    if (config.retentionCount > 100) {
        error = "Invalid backup retention value (maximum 100)";
        return false;
    }
    return true;
}

bool saveAppConfig(
    const std::string& path,
    AppConfig& config,
    std::string& error) {
    error.clear();
    if (!validateAppConfig(config, error)) {
        return false;
    }

    std::string protectedCredential;
    if (!protectNextcloudCredential(
            config.nextcloudAppPassword,
            protectedCredential,
            error)) {
        return false;
    }

    ensureConfigDirectory();
    const std::string temporaryPath = path + ".new";
    const std::string backupPath = path + ".bak";
    std::remove(temporaryPath.c_str());
    if (!writeConfigFile(temporaryPath, config, protectedCredential)) {
        const int writeError = errno;
        secureClearString(protectedCredential);
        std::remove(temporaryPath.c_str());
        error = "Unable to write config.ini (errno "
            + std::to_string(writeError) + ")";
        return false;
    }

    scrubAndRemoveFile(backupPath);
    bool hadOriginal = false;
    {
        std::ifstream original(path);
        hadOriginal = original.good();
    }
    if (hadOriginal && std::rename(path.c_str(), backupPath.c_str()) != 0) {
        const int renameError = errno;
        secureClearString(protectedCredential);
        std::remove(temporaryPath.c_str());
        error = "Unable to prepare config.ini (errno "
            + std::to_string(renameError) + ")";
        return false;
    }
    if (std::rename(temporaryPath.c_str(), path.c_str()) != 0) {
        const int renameError = errno;
        secureClearString(protectedCredential);
        if (hadOriginal) {
            std::rename(backupPath.c_str(), path.c_str());
        }
        std::remove(temporaryPath.c_str());
        error = "Unable to replace config.ini (errno "
            + std::to_string(renameError) + ")";
        return false;
    }
    secureClearString(protectedCredential);
    scrubAndRemoveFile(backupPath);
    config.credentialEncrypted = true;
    config.credentialError.clear();
    return true;
}

} // namespace nxsync
