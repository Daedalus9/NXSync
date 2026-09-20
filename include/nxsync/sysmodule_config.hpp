#pragma once

#include <cstdint>
#include <string>

namespace nxsync {

constexpr unsigned SysmoduleConfigVersion = 1;

struct SysmoduleConfig {
    unsigned version{SysmoduleConfigVersion};
    bool enabled{false};
    bool preflightEnabled{false};
    // Older enabled configurations always backed up games on exit.
    bool backupOnGameExit{true};
    bool emummcOnly{true};
    std::uint32_t pollIntervalSeconds{15};
};

bool validateSysmoduleConfig(const SysmoduleConfig& config, std::string& error);
std::string serializeSysmoduleConfig(const SysmoduleConfig& config);
bool parseSysmoduleConfig(
    const std::string& text,
    SysmoduleConfig& config,
    std::string& error);
bool loadSysmoduleConfig(
    const std::string& path,
    SysmoduleConfig& config,
    std::string& error);
bool writeSysmoduleConfig(
    const std::string& path,
    const SysmoduleConfig& config,
    int& systemError);

} // namespace nxsync
