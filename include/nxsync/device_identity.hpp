#pragma once

#include <array>
#include <optional>
#include <string>

namespace nxsync {

enum class IdentitySource {
    MacAddress,
    PersistentFallback,
    ConfigOverride,
};

struct DeviceIdentity {
    std::string model;
    std::string nickname;
    std::string suffix;
    std::string folderName;
    IdentitySource source{IdentitySource::PersistentFallback};
    bool modelDetected{false};
};

std::string modelSlugFromRaw(int rawModel);
std::string formatMacSuffix(const std::array<unsigned char, 6>& mac);
std::string sanitizePathSegment(const std::string& value);
std::string identitySourceLabel(IdentitySource source);

DeviceIdentity detectDeviceIdentity(
    const std::string& overrideId,
    const std::string& fallbackIdPath);

} // namespace nxsync

