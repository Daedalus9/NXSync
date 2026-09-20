#include "nxsync_installer/core.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace nxsync::installer {
namespace {

std::string trim(const std::string& input) {
    std::size_t first = 0;
    while (first < input.size()
        && std::isspace(static_cast<unsigned char>(input[first])) != 0) {
        ++first;
    }

    std::size_t last = input.size();
    while (last > first
        && std::isspace(static_cast<unsigned char>(input[last - 1])) != 0) {
        --last;
    }
    return input.substr(first, last - first);
}

bool parsePositiveCount(const std::string& value, std::size_t& count) {
    if (value.empty() || value.size() > 3) return false;
    std::size_t parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
    }
    if (parsed == 0 || parsed > 16) return false;
    count = parsed;
    return true;
}

std::vector<std::string> splitHashes(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, ',')) {
        part = normalizeSha256(trim(part));
        if (!part.empty()) result.push_back(part);
    }
    return result;
}

bool safePayloadPath(const std::string& path) {
    return path.rfind("payloads/", 0) == 0
        && path.find("..") == std::string::npos
        && path.find('\\') == std::string::npos
        && path.size() > std::string("payloads/").size();
}

bool safeCommonPath(const std::string& path) {
    return path.rfind("common/", 0) == 0
        && path.find("..") == std::string::npos
        && path.find('\\') == std::string::npos
        && path.size() > std::string("common/").size();
}

} // namespace

std::string normalizeSha256(const std::string& value) {
    std::string normalized = trim(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

bool isSha256Hex(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool parsePayloadManifest(
    const std::string& text,
    PayloadManifest& manifest,
    std::string& error) {
    manifest = {};
    error.clear();

    std::map<std::string, std::string> values;
    std::stringstream stream(text);
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            error = "Manifest line without '=': " + std::to_string(lineNumber);
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || values.find(key) != values.end()) {
            error = "Invalid or duplicate manifest key: " + key;
            return false;
        }
        values.emplace(key, value);
    }

    if (values["schema"] != "nxsync-installer-payloads"
        || values["version"] != "2") {
        error = "Unsupported installer manifest schema";
        return false;
    }

    std::size_t commonCount = 0;
    if (!parsePositiveCount(values["common.count"], commonCount)) {
        error = "Invalid common payload count";
        return false;
    }

    std::set<std::string> commonPaths;
    for (std::size_t index = 0; index < commonCount; ++index) {
        const std::string prefix = "common." + std::to_string(index) + ".";
        CommonPayloadDescriptor common;
        common.path = values[prefix + "path"];
        common.sha256 = normalizeSha256(values[prefix + "sha256"]);
        if (!safeCommonPath(common.path) || !isSha256Hex(common.sha256)
            || !commonPaths.insert(common.path).second) {
            error = "Invalid common payload: " + std::to_string(index);
            return false;
        }
        manifest.commonPayloads.push_back(std::move(common));
    }

    std::size_t count = 0;
    if (!parsePositiveCount(values["count"], count)) {
        error = "Invalid payload count";
        return false;
    }

    std::set<std::string> ids;
    std::set<std::string> packageHashes;
    for (std::size_t index = 0; index < count; ++index) {
        const std::string prefix = "payload." + std::to_string(index) + ".";
        PayloadDescriptor payload;
        payload.id = values[prefix + "id"];
        payload.atmosphereVersion = values[prefix + "atmosphere_version"];
        payload.package3Sha256 = normalizeSha256(
            values[prefix + "package3_sha256"]);
        payload.officialRomfsSha256 = normalizeSha256(
            values[prefix + "official_romfs_sha256"]);
        payload.integratedRomfsSha256 = normalizeSha256(
            values[prefix + "integrated_romfs_sha256"]);
        payload.dmntOverrideSha256 = normalizeSha256(
            values[prefix + "dmnt_override_sha256"]);
        payload.previousDmntOverrideSha256 = splitHashes(
            values[prefix + "previous_dmnt_override_sha256"]);
        payload.dmntPath = values[prefix + "dmnt_path"];

        if (payload.id.empty() || payload.atmosphereVersion.empty()
            || !isSha256Hex(payload.package3Sha256)
            || !isSha256Hex(payload.officialRomfsSha256)
            || !isSha256Hex(payload.integratedRomfsSha256)
            || !isSha256Hex(payload.dmntOverrideSha256)
            || !safePayloadPath(payload.dmntPath)) {
            error = "Invalid manifest payload: " + std::to_string(index);
            return false;
        }
        for (const auto& hash : payload.previousDmntOverrideSha256) {
            if (!isSha256Hex(hash)) {
                error = "Invalid previous hash in payload: "
                    + std::to_string(index);
                return false;
            }
        }
        if (!ids.insert(payload.id).second
            || !packageHashes.insert(payload.package3Sha256).second) {
            error = "Duplicate ID or package3 in the manifest";
            return false;
        }
        manifest.payloads.push_back(std::move(payload));
    }

    const std::size_t expectedKeys = 4 + commonCount * 2 + count * 8;
    if (values.size() != expectedKeys) {
        error = "The manifest contains missing or unsupported keys";
        manifest = {};
        return false;
    }

    return true;
}

CompatibilityResult classifyInstallation(
    const std::vector<PayloadDescriptor>& payloads,
    const std::string& package3Sha256,
    const std::string& stratosphereRomfsSha256,
    const std::string& dmntOverrideSha256) {
    const std::string packageHash = normalizeSha256(package3Sha256);
    const std::string romfsHash = normalizeSha256(stratosphereRomfsSha256);
    const std::string dmntHash = normalizeSha256(dmntOverrideSha256);

    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const PayloadDescriptor& payload = payloads[index];
        if (payload.package3Sha256 != packageHash) continue;

        if (payload.integratedRomfsSha256 == romfsHash) {
            return {
                CompatibilityState::IncompatibleRomfs,
                index,
                "Integrated NXSync ROMFS detected: restore the original "
                    "Atmosphere files from a PC"};
        }
        if (payload.officialRomfsSha256 != romfsHash) {
            return {
                CompatibilityState::IncompatibleRomfs,
                index,
                "package3 recognized, but stratosphere.romfs does not match"};
        }

        if (dmntHash.empty()) {
            return {
                CompatibilityState::ReadyToInstall,
                index,
                "Compatibility verified: installation available"};
        }
        if (payload.dmntOverrideSha256 == dmntHash) {
            return {
                CompatibilityState::InstalledCurrent,
                index,
                "NXSync components are already installed"};
        }
        if (std::find(
                payload.previousDmntOverrideSha256.begin(),
                payload.previousDmntOverrideSha256.end(),
                dmntHash) != payload.previousDmntOverrideSha256.end()) {
            return {
                CompatibilityState::UpgradeAvailable,
                index,
                "NXSync update available"};
        }
        return {
            CompatibilityState::UnknownDmntOverride,
            index,
            "Existing dmnt override is not recognized: installation blocked"};
    }

    return {
        CompatibilityState::UnknownPackage3,
        static_cast<std::size_t>(-1),
        "Unsupported Atmosphere/package3 version"};
}

const char* compatibilityStateName(const CompatibilityState state) {
    switch (state) {
        case CompatibilityState::UnknownPackage3: return "unknown-package3";
        case CompatibilityState::IncompatibleRomfs: return "incompatible-romfs";
        case CompatibilityState::UnknownDmntOverride: return "unknown-dmnt-override";
        case CompatibilityState::ReadyToInstall: return "ready";
        case CompatibilityState::UpgradeAvailable: return "upgrade";
        case CompatibilityState::InstalledCurrent: return "installed";
    }
    return "unknown";
}

} // namespace nxsync::installer
