#pragma once

#include <cstdint>
#include <string>

namespace nxsync {

enum class StorageEnvironment {
    Unknown,
    SysMmc,
    EmuMmc,
};

StorageEnvironment parseAtmosphereStorageEnvironment(
    const std::string& displayVersion);
StorageEnvironment classifyAtmosphereEmummcConfig(
    std::uint32_t magic,
    std::uint32_t type);
StorageEnvironment classifyAtmosphereEmummcType(std::uint64_t type);
StorageEnvironment detectCurrentStorageEnvironment();
const char* storageEnvironmentKey(StorageEnvironment environment);
bool automationScopeAllows(
    bool emummcOnly,
    StorageEnvironment environment);
bool sysmoduleObserverShouldRun(
    bool enabled,
    bool emummcOnly,
    StorageEnvironment environment);

} // namespace nxsync
