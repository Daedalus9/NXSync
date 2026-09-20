#include "nxsync_installer/core.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

constexpr const char* HashA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* HashB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* HashC =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr const char* HashD =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr const char* HashE =
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

std::string validManifest() {
    return std::string()
        + "schema=nxsync-installer-payloads\n"
        + "version=2\n"
        + "common.count=1\n"
        + "common.0.path=common/switch/NXSync/NXSync.nro\n"
        + "common.0.sha256=" + HashE + "\n"
        + "count=1\n"
        + "payload.0.id=ams-1.11.2\n"
        + "payload.0.atmosphere_version=1.11.2\n"
        + "payload.0.package3_sha256=" + HashA + "\n"
        + "payload.0.official_romfs_sha256=" + HashB + "\n"
        + "payload.0.integrated_romfs_sha256=" + HashC + "\n"
        + "payload.0.dmnt_override_sha256=" + HashD + "\n"
        + "payload.0.previous_dmnt_override_sha256=" + HashE + "\n"
        + "payload.0.dmnt_path=payloads/1.11.2/dmnt.nsp\n";
}

} // namespace

int main() {
    nxsync::installer::PayloadManifest manifest;
    std::string error;
    assert(nxsync::installer::parsePayloadManifest(
        validManifest(), manifest, error));
    const auto& payloads = manifest.payloads;
    assert(manifest.commonPayloads.size() == 1);
    assert(manifest.commonPayloads[0].sha256 == HashE);
    assert(payloads.size() == 1);
    assert(payloads[0].atmosphereVersion == "1.11.2");

    auto result = nxsync::installer::classifyInstallation(
        payloads, HashA, HashB, "");
    assert(result.state == nxsync::installer::CompatibilityState::ReadyToInstall);
    assert(result.payloadIndex == 0);

    result = nxsync::installer::classifyInstallation(payloads, HashA, HashB, HashD);
    assert(result.state == nxsync::installer::CompatibilityState::InstalledCurrent);

    result = nxsync::installer::classifyInstallation(payloads, HashA, HashB, HashE);
    assert(result.state == nxsync::installer::CompatibilityState::UpgradeAvailable);

    result = nxsync::installer::classifyInstallation(payloads, HashA, HashC, "");
    assert(result.state == nxsync::installer::CompatibilityState::IncompatibleRomfs);

    result = nxsync::installer::classifyInstallation(payloads, HashA, HashB, HashC);
    assert(result.state == nxsync::installer::CompatibilityState::UnknownDmntOverride);

    result = nxsync::installer::classifyInstallation(payloads, HashE, HashB, "");
    assert(result.state == nxsync::installer::CompatibilityState::UnknownPackage3);

    std::string invalid = validManifest();
    invalid.replace(invalid.find(HashC), 64, "not-a-hash");
    assert(!nxsync::installer::parsePayloadManifest(invalid, manifest, error));

    invalid = validManifest()
        + "payload.0.id=duplicate\n";
    assert(!nxsync::installer::parsePayloadManifest(invalid, manifest, error));

    invalid = validManifest() + "unexpected=value\n";
    assert(!nxsync::installer::parsePayloadManifest(invalid, manifest, error));

    invalid = validManifest();
    invalid.replace(
        invalid.find("common/switch/NXSync/NXSync.nro"),
        std::string("common/switch/NXSync/NXSync.nro").size(),
        "common/../escape");
    assert(!nxsync::installer::parsePayloadManifest(invalid, manifest, error));

    return 0;
}
