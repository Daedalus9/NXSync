#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nxsync::installer {

struct PayloadDescriptor {
    std::string id;
    std::string atmosphereVersion;
    std::string package3Sha256;
    std::string officialRomfsSha256;
    std::string integratedRomfsSha256;
    std::string dmntOverrideSha256;
    std::vector<std::string> previousDmntOverrideSha256;
    std::string dmntPath;
};

struct CommonPayloadDescriptor {
    std::string path;
    std::string sha256;
};

struct PayloadManifest {
    std::vector<CommonPayloadDescriptor> commonPayloads;
    std::vector<PayloadDescriptor> payloads;
};

enum class CompatibilityState {
    UnknownPackage3,
    IncompatibleRomfs,
    UnknownDmntOverride,
    ReadyToInstall,
    UpgradeAvailable,
    InstalledCurrent,
};

struct CompatibilityResult {
    CompatibilityState state{CompatibilityState::UnknownPackage3};
    std::size_t payloadIndex{static_cast<std::size_t>(-1)};
    std::string message;

    bool supported() const {
        return state == CompatibilityState::ReadyToInstall
            || state == CompatibilityState::UpgradeAvailable
            || state == CompatibilityState::InstalledCurrent;
    }
};

bool parsePayloadManifest(
    const std::string& text,
    PayloadManifest& manifest,
    std::string& error);

CompatibilityResult classifyInstallation(
    const std::vector<PayloadDescriptor>& payloads,
    const std::string& package3Sha256,
    const std::string& stratosphereRomfsSha256,
    const std::string& dmntOverrideSha256);

bool isSha256Hex(const std::string& value);
std::string normalizeSha256(const std::string& value);
const char* compatibilityStateName(CompatibilityState state);

} // namespace nxsync::installer
