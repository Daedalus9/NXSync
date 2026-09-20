#include "nxsync/storage_environment.hpp"

#include <cctype>

#ifdef __SWITCH__
#include <switch.h>
#include <cstring>
#endif

namespace nxsync {

namespace {
constexpr std::uint32_t AtmosphereEmummcMagic = 0x30534645; // EFS0
}

StorageEnvironment parseAtmosphereStorageEnvironment(
    const std::string& displayVersion) {
    std::size_t end = displayVersion.size();
    while (end > 0 && std::isspace(
        static_cast<unsigned char>(displayVersion[end - 1]))) {
        --end;
    }
    if (end < 2 || displayVersion[end - 2] != '|') {
        return StorageEnvironment::Unknown;
    }
    if (displayVersion[end - 1] == 'E') return StorageEnvironment::EmuMmc;
    if (displayVersion[end - 1] == 'S') return StorageEnvironment::SysMmc;
    return StorageEnvironment::Unknown;
}

StorageEnvironment classifyAtmosphereEmummcConfig(
    const std::uint32_t magic,
    const std::uint32_t type) {
    if (magic != AtmosphereEmummcMagic) {
        return StorageEnvironment::Unknown;
    }
    if (type == 0) return StorageEnvironment::SysMmc;
    if (type == 1 || type == 2) return StorageEnvironment::EmuMmc;
    return StorageEnvironment::Unknown;
}

StorageEnvironment classifyAtmosphereEmummcType(const std::uint64_t type) {
    return type == 0
        ? StorageEnvironment::SysMmc
        : StorageEnvironment::EmuMmc;
}

StorageEnvironment detectCurrentStorageEnvironment() {
#ifdef __SWITCH__
    const Result splResult = splInitialize();
    if (R_SUCCEEDED(splResult)) {
        u64 emummcType = 0;
        const Result configResult = splGetConfig(
            static_cast<SplConfigItem>(65007), &emummcType);
        splExit();
        if (R_SUCCEEDED(configResult)) {
            return classifyAtmosphereEmummcType(emummcType);
        }
    }
    const Result setResult = setsysInitialize();
    if (R_SUCCEEDED(setResult)) {
        SetSysFirmwareVersion firmware{};
        const Result versionResult = setsysGetFirmwareVersion(&firmware);
        setsysExit();
        if (R_SUCCEEDED(versionResult)) {
            return parseAtmosphereStorageEnvironment(std::string(
                firmware.display_version,
                strnlen(
                    firmware.display_version,
                    sizeof(firmware.display_version))));
        }
    }
#endif
    return StorageEnvironment::Unknown;
}

const char* storageEnvironmentKey(const StorageEnvironment environment) {
    switch (environment) {
        case StorageEnvironment::EmuMmc: return "emummc";
        case StorageEnvironment::SysMmc: return "sysmmc";
        case StorageEnvironment::Unknown:
        default: return "unknown";
    }
}

bool automationScopeAllows(
    const bool emummcOnly,
    const StorageEnvironment environment) {
    if (environment == StorageEnvironment::Unknown) return false;
    return !emummcOnly || environment == StorageEnvironment::EmuMmc;
}

bool sysmoduleObserverShouldRun(
    const bool enabled,
    const bool emummcOnly,
    const StorageEnvironment environment) {
    return enabled && automationScopeAllows(emummcOnly, environment);
}

} // namespace nxsync
