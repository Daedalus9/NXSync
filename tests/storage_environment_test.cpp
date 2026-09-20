#include "nxsync/storage_environment.hpp"

#include <cassert>

int main() {
    using nxsync::StorageEnvironment;
    assert(nxsync::parseAtmosphereStorageEnvironment(
        "21.2.0|AMS 1.11.2|E") == StorageEnvironment::EmuMmc);
    assert(nxsync::parseAtmosphereStorageEnvironment(
        "21.2.0|AMS 1.11.2|S") == StorageEnvironment::SysMmc);
    assert(nxsync::parseAtmosphereStorageEnvironment(
        "21.2.0|AMS 1.11.2|E  ") == StorageEnvironment::EmuMmc);
    assert(nxsync::parseAtmosphereStorageEnvironment(
        "21.2.0") == StorageEnvironment::Unknown);
    assert(nxsync::classifyAtmosphereEmummcConfig(
        0x30534645, 0) == StorageEnvironment::SysMmc);
    assert(nxsync::classifyAtmosphereEmummcConfig(
        0x30534645, 1) == StorageEnvironment::EmuMmc);
    assert(nxsync::classifyAtmosphereEmummcConfig(
        0x30534645, 2) == StorageEnvironment::EmuMmc);
    assert(nxsync::classifyAtmosphereEmummcConfig(
        0, 0) == StorageEnvironment::Unknown);
    assert(nxsync::classifyAtmosphereEmummcConfig(
        0x30534645, 3) == StorageEnvironment::Unknown);
    assert(nxsync::classifyAtmosphereEmummcType(0)
        == StorageEnvironment::SysMmc);
    assert(nxsync::classifyAtmosphereEmummcType(1)
        == StorageEnvironment::EmuMmc);
    assert(nxsync::classifyAtmosphereEmummcType(2)
        == StorageEnvironment::EmuMmc);
    assert(nxsync::automationScopeAllows(true, StorageEnvironment::EmuMmc));
    assert(!nxsync::automationScopeAllows(true, StorageEnvironment::SysMmc));
    assert(!nxsync::automationScopeAllows(true, StorageEnvironment::Unknown));
    assert(nxsync::automationScopeAllows(false, StorageEnvironment::SysMmc));
    assert(!nxsync::automationScopeAllows(false, StorageEnvironment::Unknown));
    assert(nxsync::sysmoduleObserverShouldRun(
        true, true, StorageEnvironment::EmuMmc));
    assert(!nxsync::sysmoduleObserverShouldRun(
        true, true, StorageEnvironment::SysMmc));
    assert(!nxsync::sysmoduleObserverShouldRun(
        true, true, StorageEnvironment::Unknown));
    assert(nxsync::sysmoduleObserverShouldRun(
        true, false, StorageEnvironment::SysMmc));
    assert(!nxsync::sysmoduleObserverShouldRun(
        false, false, StorageEnvironment::EmuMmc));
    return 0;
}
