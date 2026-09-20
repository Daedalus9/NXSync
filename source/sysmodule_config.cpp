#include "nxsync/atomic_file.hpp"
#include "nxsync/sysmodule_config.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace nxsync {
namespace {

std::string trim(std::string value) {
    const auto visible = [](const unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), visible));
    value.erase(std::find_if(value.rbegin(), value.rend(), visible).base(), value.end());
    return value;
}

bool parseUnsigned(const std::string& value, std::uint32_t& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0'
        || parsed > 0xFFFFFFFFUL) {
        return false;
    }
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

} // namespace

bool validateSysmoduleConfig(
    const SysmoduleConfig& config,
    std::string& error) {
    if (config.version != SysmoduleConfigVersion) {
        error = "Unsupported sysmodule configuration version";
        return false;
    }
    if (config.pollIntervalSeconds < 5 || config.pollIntervalSeconds > 3600) {
        error = "Sysmodule interval is outside the allowed range (5-3600 seconds)";
        return false;
    }
    error.clear();
    return true;
}

std::string serializeSysmoduleConfig(const SysmoduleConfig& config) {
    return "# NXSync sysmodule - background automation\n"
        "version=" + std::to_string(config.version) + "\n"
        "enabled=" + std::string(config.enabled ? "true" : "false") + "\n"
        "preflight_enabled="
            + std::string(config.preflightEnabled ? "true" : "false") + "\n"
        "backup_on_game_exit="
            + std::string(config.backupOnGameExit ? "true" : "false") + "\n"
        "automation_scope="
            + std::string(config.emummcOnly ? "emummc" : "all") + "\n"
        "poll_interval_seconds=" + std::to_string(config.pollIntervalSeconds) + "\n";
}

bool parseSysmoduleConfig(
    const std::string& text,
    SysmoduleConfig& config,
    std::string& error) {
    config = SysmoduleConfig{};
    bool versionSeen = false;
    bool enabledSeen = false;
    bool intervalSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string line = trim(text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos) {
                error = "Invalid line in sysmodule configuration";
                return false;
            }
            const std::string key = trim(line.substr(0, separator));
            std::string value = trim(line.substr(separator + 1));
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            std::uint32_t number = 0;
            if (key == "version") {
                if (!parseUnsigned(value, number)) {
                    error = "Invalid sysmodule version";
                    return false;
                }
                config.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "enabled") {
                if (value != "true" && value != "false") {
                    error = "Invalid enabled value";
                    return false;
                }
                config.enabled = value == "true";
                enabledSeen = true;
            } else if (key == "preflight_enabled") {
                if (value != "true" && value != "false") {
                    error = "Invalid preflight_enabled value";
                    return false;
                }
                config.preflightEnabled = value == "true";
            } else if (key == "backup_on_game_exit") {
                if (value != "true" && value != "false") {
                    error = "Invalid backup_on_game_exit value";
                    return false;
                }
                config.backupOnGameExit = value == "true";
            } else if (key == "automation_scope") {
                if (value != "emummc" && value != "all") {
                    error = "Invalid automation_scope value";
                    return false;
                }
                config.emummcOnly = value == "emummc";
            } else if (key == "poll_interval_seconds") {
                if (!parseUnsigned(value, config.pollIntervalSeconds)) {
                    error = "Invalid sysmodule interval";
                    return false;
                }
                intervalSeen = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !enabledSeen || !intervalSeen) {
        error = "Required sysmodule configuration fields are missing";
        return false;
    }
    return validateSysmoduleConfig(config, error);
}

bool loadSysmoduleConfig(
    const std::string& path,
    SysmoduleConfig& config,
    std::string& error) {
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError, 4 * 1024 * 1024)) {
        error = "Sysmodule configuration was not found";
        return false;
    }
    return parseSysmoduleConfig(text, config, error);
}

bool writeSysmoduleConfig(
    const std::string& path,
    const SysmoduleConfig& config,
    int& systemError) {
    std::string validationError;
    if (!validateSysmoduleConfig(config, validationError)) {
        systemError = EINVAL;
        return false;
    }
    return writeTextFileAtomic(path, serializeSysmoduleConfig(config), systemError);
}

} // namespace nxsync
