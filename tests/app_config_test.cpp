#include "nxsync/app_config.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    nxsync::AppConfig config;
    assert(config.retentionCount == 5);
    config.remoteRoot = "Switch backups";
    config.deviceIdOverride = "NS-OLED-TEST";
    config.nextcloudUrl = "https://cloud.example.com/nextcloud";
    config.nextcloudUsername = "mario";
    config.nextcloudAppPassword = "dedicated-password";
    config.autoBackupOnStart = true;
    config.retentionCount = 5;

    std::string error;
    assert(nxsync::validateAppConfig(config, error));

    nxsync::AppConfig invalid = config;
    invalid.nextcloudUrl = "http://cloud.example.com";
    assert(!nxsync::validateAppConfig(invalid, error));
    invalid = config;
    invalid.nextcloudAppPassword.clear();
    assert(!nxsync::validateAppConfig(invalid, error));
    invalid = config;
    invalid.nextcloudUsername = "bad\nuser";
    assert(!nxsync::validateAppConfig(invalid, error));
    invalid = config;
    invalid.retentionCount = 101;
    assert(!nxsync::validateAppConfig(invalid, error));

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "nxsync-app-config-test.ini";
    const std::string pathString = path.string();
    std::remove(pathString.c_str());
    std::remove((pathString + ".new").c_str());
    std::remove((pathString + ".bak").c_str());

    assert(nxsync::saveAppConfig(pathString, config, error));
    assert(config.credentialEncrypted);
    assert(config.credentialError.empty());
    std::string encryptedContents;
    {
        std::ifstream encryptedFile(pathString);
        encryptedContents.assign(
            std::istreambuf_iterator<char>(encryptedFile),
            std::istreambuf_iterator<char>());
    }
    assert(encryptedContents.find("dedicated-password") == std::string::npos);
    assert(encryptedContents.find("nextcloud_app_password=dedicated-password")
        == std::string::npos);
    assert(encryptedContents.find("nextcloud_app_password_encrypted=v1:")
        != std::string::npos);
    const nxsync::AppConfig loaded = nxsync::loadOrCreateConfig(pathString);
    assert(loaded.remoteRoot == config.remoteRoot);
    assert(loaded.deviceIdOverride == config.deviceIdOverride);
    assert(loaded.nextcloudUrl == config.nextcloudUrl);
    assert(loaded.nextcloudUsername == config.nextcloudUsername);
    assert(loaded.nextcloudAppPassword == config.nextcloudAppPassword);
    assert(loaded.credentialEncrypted);
    assert(loaded.credentialError.empty());
    assert(loaded.autoBackupOnStart);
    assert(loaded.retentionCount == 5);

    assert(std::rename(pathString.c_str(), (pathString + ".bak").c_str()) == 0);
    const nxsync::AppConfig recovered = nxsync::loadOrCreateConfig(pathString);
    assert(recovered.nextcloudUrl == config.nextcloudUrl);
    assert(recovered.autoBackupOnStart);
    assert(recovered.retentionCount == 5);
    assert(std::filesystem::exists(path));

    const std::filesystem::path tamperedPath =
        std::filesystem::temp_directory_path() / "nxsync-app-config-tampered-test.ini";
    const std::string tamperedPathString = tamperedPath.string();
    std::string tamperedContents = encryptedContents;
    const std::size_t credentialLine = tamperedContents.find(
        "nextcloud_app_password_encrypted=v1:");
    assert(credentialLine != std::string::npos);
    const std::size_t lineEnd = tamperedContents.find('\n', credentialLine);
    assert(lineEnd != std::string::npos && lineEnd > credentialLine);
    tamperedContents[lineEnd - 1] = tamperedContents[lineEnd - 1] == '0' ? '1' : '0';
    {
        std::ofstream tamperedFile(tamperedPathString, std::ios::trunc);
        tamperedFile << tamperedContents;
    }
    const nxsync::AppConfig tampered =
        nxsync::loadOrCreateConfig(tamperedPathString);
    assert(tampered.nextcloudAppPassword.empty());
    assert(tampered.credentialEncrypted);
    assert(!tampered.credentialError.empty());

    const std::filesystem::path legacyPath =
        std::filesystem::temp_directory_path() / "nxsync-app-config-legacy-test.ini";
    const std::string legacyPathString = legacyPath.string();
    {
        std::ofstream legacy(legacyPathString, std::ios::trunc);
        legacy
            << "remote_root=NXSync\n"
            << "nextcloud_url=https://cloud.example.com/nextcloud\n"
            << "nextcloud_username=mario\n"
            << "nextcloud_app_password=dedicated-password\n";
    }
    const nxsync::AppConfig migrated = nxsync::loadOrCreateConfig(legacyPathString);
    assert(migrated.nextcloudAppPassword == "dedicated-password");
    assert(migrated.credentialEncrypted);
    assert(migrated.credentialMigrated);
    assert(migrated.credentialError.empty());
    assert(!migrated.autoBackupOnStart);
    assert(migrated.retentionCount == 5);
    {
        std::ifstream migratedFile(legacyPathString);
        const std::string contents{
            std::istreambuf_iterator<char>(migratedFile),
            std::istreambuf_iterator<char>()};
        assert(contents.find("auto_backup_on_start=false") != std::string::npos);
        assert(contents.find("retention_count=5") != std::string::npos);
        assert(contents.find("nextcloud_app_password_encrypted=v1:")
            != std::string::npos);
        assert(contents.find("nextcloud_app_password=dedicated-password")
            == std::string::npos);
        assert(contents.find("dedicated-password") == std::string::npos);
    }

    std::remove(pathString.c_str());
    std::remove((pathString + ".new").c_str());
    std::remove((pathString + ".bak").c_str());
    std::remove(tamperedPathString.c_str());
    std::remove((tamperedPathString + ".new").c_str());
    std::remove((tamperedPathString + ".bak").c_str());
    std::remove(legacyPathString.c_str());
    return 0;
}
