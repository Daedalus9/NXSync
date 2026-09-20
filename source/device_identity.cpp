#include "nxsync/device_identity.hpp"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>

namespace nxsync {
namespace {

constexpr int ProductModelInvalid = 0;
constexpr int ProductModelNx = 1;
constexpr int ProductModelCopper = 2;
constexpr int ProductModelIowa = 3;
constexpr int ProductModelHoag = 4;
constexpr int ProductModelCalcio = 5;
constexpr int ProductModelAula = 6;

bool isUsableSuffix(const std::string& value) {
    if (value.size() != 6) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

std::string loadOrCreateFallbackSuffix(const std::string& path) {
    std::ifstream input(path);
    std::string stored;
    if (input && std::getline(input, stored)) {
        stored.erase(std::remove_if(stored.begin(), stored.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), stored.end());
        std::transform(stored.begin(), stored.end(), stored.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (isUsableSuffix(stored)) {
            return stored;
        }
    }

    std::array<unsigned char, 3> bytes{};
    randomGet(bytes.data(), bytes.size());

    char buffer[7]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02X%02X%02X",
        static_cast<unsigned>(bytes[0]),
        static_cast<unsigned>(bytes[1]),
        static_cast<unsigned>(bytes[2]));

    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/NXSync", 0777);
    std::ofstream output(path, std::ios::trunc);
    if (output) {
        output << buffer << '\n';
    }
    return buffer;
}

std::optional<std::array<unsigned char, 6>> readWirelessMac() {
    if (R_FAILED(setcalInitialize())) {
        return std::nullopt;
    }

    SetCalMacAddress address{};
    const Result result = setcalGetWirelessLanMacAddress(&address);
    setcalExit();
    if (R_FAILED(result)) {
        return std::nullopt;
    }

    std::array<unsigned char, 6> bytes{};
    std::copy(std::begin(address.addr), std::end(address.addr), bytes.begin());
    return bytes;
}

std::string readNickname() {
    if (R_FAILED(setInitialize())) {
        return {};
    }

    SetSysDeviceNickName nickname{};
    const Result result = setGetDeviceNickname(&nickname);
    setExit();
    if (R_FAILED(result)) {
        return {};
    }
    return nickname.nickname;
}

std::optional<int> readProductModel() {
    if (R_FAILED(setsysInitialize())) {
        return std::nullopt;
    }

    SetSysProductModel model = SetSysProductModel_Invalid;
    const Result result = setsysGetProductModel(&model);
    setsysExit();
    if (R_FAILED(result)) {
        return std::nullopt;
    }
    return static_cast<int>(model);
}

} // namespace

std::string modelSlugFromRaw(const int rawModel) {
    switch (rawModel) {
        case ProductModelNx:
            return "NS";
        case ProductModelIowa:
            return "NS-V2";
        case ProductModelHoag:
            return "NS-LITE";
        case ProductModelAula:
            return "NS-OLED";
        case ProductModelCopper:
        case ProductModelCalcio:
            return "NS-SIM";
        case ProductModelInvalid:
        default:
            return "NS";
    }
}

std::string formatMacSuffix(const std::array<unsigned char, 6>& mac) {
    char buffer[7]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02X%02X%02X",
        static_cast<unsigned>(mac[3]),
        static_cast<unsigned>(mac[4]),
        static_cast<unsigned>(mac[5]));
    return buffer;
}

std::string sanitizePathSegment(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(std::min<std::size_t>(value.size(), 64));
    bool previousWasSeparator = false;

    for (const unsigned char ch : value) {
        if (sanitized.size() >= 64) {
            break;
        }

        if (std::isalnum(ch)) {
            sanitized.push_back(static_cast<char>(std::toupper(ch)));
            previousWasSeparator = false;
        } else if (!previousWasSeparator && !sanitized.empty()) {
            sanitized.push_back('-');
            previousWasSeparator = true;
        }
    }

    while (!sanitized.empty() && sanitized.back() == '-') {
        sanitized.pop_back();
    }
    return sanitized;
}

std::string identitySourceLabel(const IdentitySource source) {
    switch (source) {
        case IdentitySource::MacAddress:
            return "MAC Wi-Fi";
        case IdentitySource::ConfigOverride:
            return "configuration";
        case IdentitySource::PersistentFallback:
        default:
            return "persistent local ID";
    }
}

DeviceIdentity detectDeviceIdentity(
    const std::string& overrideId,
    const std::string& fallbackIdPath) {
    DeviceIdentity identity;
    identity.nickname = readNickname();

    const auto productModel = readProductModel();
    identity.modelDetected = productModel.has_value();
    identity.model = modelSlugFromRaw(productModel.value_or(ProductModelInvalid));

    const std::string sanitizedOverride = sanitizePathSegment(overrideId);
    if (!sanitizedOverride.empty()) {
        identity.folderName = sanitizedOverride;
        identity.suffix.clear();
        identity.source = IdentitySource::ConfigOverride;
        return identity;
    }

    const auto mac = readWirelessMac();
    if (mac.has_value()) {
        identity.suffix = formatMacSuffix(*mac);
        identity.source = IdentitySource::MacAddress;
    } else {
        identity.suffix = loadOrCreateFallbackSuffix(fallbackIdPath);
        identity.source = IdentitySource::PersistentFallback;
    }

    identity.folderName = sanitizePathSegment(identity.model + "-" + identity.suffix);
    return identity;
}

} // namespace nxsync
