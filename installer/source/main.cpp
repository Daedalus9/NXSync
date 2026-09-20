#include "nxsync_installer/filesystem.hpp"
#include <switch.h>

#include "nxsync_installer/core.hpp"
#include "nxsync/overlay_dependencies.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* InstallerVersion = "0.1.13-rc2";
constexpr const char* ManifestPath = "romfs:/payloads/manifest.ini";
constexpr const char* Package3Path = "sdmc:/atmosphere/package3";
constexpr const char* StratosphereRomfsPath =
    "sdmc:/atmosphere/stratosphere.romfs";
constexpr const char* DmntOverridePath =
    "sdmc:/atmosphere/contents/010000000000000D/exefs.nsp";
constexpr const char* InstallerMarkerPath =
    "sdmc:/config/NXSync/installer.lock";

struct CommonPayload {
    const char* source;
    const char* target;
    bool preserveExisting;
};

constexpr CommonPayload CommonPayloads[] = {
    {"romfs:/common/switch/NXSync/NXSync.nro",
     "sdmc:/switch/NXSync/NXSync.nro", false},
    {"romfs:/common/switch/.overlays/NXSync.ovl",
     "sdmc:/switch/.overlays/NXSync.ovl", false},
    {"romfs:/common/atmosphere/contents/4200000000004E58/exefs.nsp",
     "sdmc:/atmosphere/contents/4200000000004E58/exefs.nsp", false},
    {"romfs:/common/atmosphere/contents/4200000000004E58/flags/boot2.flag",
     "sdmc:/atmosphere/contents/4200000000004E58/flags/boot2.flag", false},
    {"romfs:/common/atmosphere/contents/4200000000004E59/exefs.nsp",
     "sdmc:/atmosphere/contents/4200000000004E59/exefs.nsp", false},
    {"romfs:/common/config/NXSync/sysmodule.ini",
     "sdmc:/config/NXSync/sysmodule.ini", true},
};

struct FileHash {
    bool ok{false};
    std::string sha256;
    std::uint64_t size{0};
    std::string error;
};

struct Detection {
    FileHash package3;
    FileHash stratosphereRomfs;
    FileHash dmntOverride;
    bool dmntOverridePresent{false};
    nxsync::installer::CompatibilityResult compatibility;
    bool payloadValid{false};
    std::string payloadError;
    nxsync::OverlayDependencyStatus overlayDependencies;
};

std::string errnoMessage(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

bool fileExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool commitSd(std::string& error) {
    const Result result = fsdevCommitDevice("sdmc");
    if (R_SUCCEEDED(result)) return true;

    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", result);
    error = std::string("Filesystem commit failed: ") + buffer;
    return false;
}

std::string bytesToHex(const unsigned char* bytes, const std::size_t count) {
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result(count * 2, '0');
    for (std::size_t index = 0; index < count; ++index) {
        result[index * 2] = Hex[(bytes[index] >> 4) & 0xF];
        result[index * 2 + 1] = Hex[bytes[index] & 0xF];
    }
    return result;
}

FileHash hashFile(const std::string& path) {
    FileHash result;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        result.error = errnoMessage("Open failed: " + path);
        return result;
    }

    Sha256Context context{};
    sha256ContextCreate(&context);
    std::array<unsigned char, 128 * 1024> buffer{};
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            sha256ContextUpdate(&context, buffer.data(), read);
            result.size += read;
        }
        if (read < buffer.size()) {
            if (std::ferror(file) != 0) {
                result.error = errnoMessage("Read failed: " + path);
                std::fclose(file);
                return result;
            }
            break;
        }
    }

    std::fclose(file);
    std::array<unsigned char, 32> digest{};
    sha256ContextGetHash(&context, digest.data());
    result.sha256 = bytesToHex(digest.data(), digest.size());
    result.ok = true;
    return result;
}

bool readTextFile(
    const std::string& path,
    std::string& text,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Unable to read " + path;
        return false;
    }
    text.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        error = "Incomplete read of " + path;
        return false;
    }
    return true;
}

bool removeIfPresent(const std::string& path, std::string& error) {
    if (std::remove(path.c_str()) == 0 || errno == ENOENT) return true;
    error = errnoMessage("Removal failed: " + path);
    return false;
}

bool renameFile(
    const std::string& from,
    const std::string& to,
    std::string& error) {
    if (std::rename(from.c_str(), to.c_str()) == 0) return true;
    error = errnoMessage("Rename failed: " + from + " -> " + to);
    return false;
}

bool copyFileToPath(
    const std::string& source,
    const std::string& destination,
    std::string& error) {
    FILE* input = std::fopen(source.c_str(), "rb");
    if (input == nullptr) {
        error = errnoMessage("Failed to open source: " + source);
        return false;
    }
    FILE* output = std::fopen(destination.c_str(), "wb");
    if (output == nullptr) {
        error = errnoMessage("Failed to open destination: " + destination);
        std::fclose(input);
        return false;
    }

    bool ok = true;
    std::array<unsigned char, 128 * 1024> buffer{};
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
        if (read > 0
            && std::fwrite(buffer.data(), 1, read, output) != read) {
            error = errnoMessage("Write failed: " + destination);
            ok = false;
            break;
        }
        if (read < buffer.size()) {
            if (std::ferror(input) != 0) {
                error = errnoMessage("Read failed: " + source);
                ok = false;
            }
            break;
        }
    }

    if (ok && std::fflush(output) != 0) {
        error = errnoMessage("Flush failed: " + destination);
        ok = false;
    }
    if (ok && fsync(fileno(output)) != 0) {
        error = errnoMessage("fsync failed: " + destination);
        ok = false;
    }
    std::fclose(output);
    std::fclose(input);
    return ok;
}

bool recoverAtomicTarget(
    const std::string& target,
    const std::string& expectedNewHash,
    std::string& error) {
    const std::string rollback = target + ".nxsync-rollback";
    if (!fileExists(rollback)) return true;

    if (!fileExists(target)) {
        if (!renameFile(rollback, target, error)) return false;
        return commitSd(error);
    }

    const FileHash targetHash = hashFile(target);
    if (!targetHash.ok) {
        error = targetHash.error;
        return false;
    }
    if (targetHash.sha256 == expectedNewHash) {
        if (!removeIfPresent(rollback, error)) return false;
        return commitSd(error);
    }

    const std::string failed = target + ".nxsync-failed";
    if (!removeIfPresent(failed, error)
        || !renameFile(target, failed, error)
        || !renameFile(rollback, target, error)) {
        return false;
    }
    if (!commitSd(error)) return false;
    return removeIfPresent(failed, error) && commitSd(error);
}

bool atomicReplaceFromFile(
    const std::string& source,
    const std::string& target,
    const std::string& expectedHash,
    std::string& error) {
    if (!recoverAtomicTarget(target, expectedHash, error)) return false;

    const std::string staged = target + ".nxsync-new";
    const std::string rollback = target + ".nxsync-rollback";
    if (!removeIfPresent(staged, error)
        || !copyFileToPath(source, staged, error)) {
        return false;
    }

    const FileHash stagedHash = hashFile(staged);
    if (!stagedHash.ok || stagedHash.sha256 != expectedHash) {
        error = stagedHash.ok
            ? "Staged file hash mismatch: " + target
            : stagedHash.error;
        removeIfPresent(staged, error);
        return false;
    }
    if (!commitSd(error)) return false;

    if (fileExists(target)) {
        if (!removeIfPresent(rollback, error)
            || !renameFile(target, rollback, error)) {
            return false;
        }
    }
    if (!renameFile(staged, target, error)) {
        if (fileExists(rollback) && !fileExists(target)) {
            std::string ignored;
            renameFile(rollback, target, ignored);
            commitSd(ignored);
        }
        return false;
    }
    if (!commitSd(error)) return false;

    const FileHash finalHash = hashFile(target);
    if (!finalHash.ok || finalHash.sha256 != expectedHash) {
        error = finalHash.ok
            ? "Final verification failed: " + target
            : finalHash.error;
        if (fileExists(rollback)) {
            const std::string failed = target + ".nxsync-failed";
            std::string ignored;
            removeIfPresent(failed, ignored);
            renameFile(target, failed, ignored);
            renameFile(rollback, target, ignored);
            commitSd(ignored);
            removeIfPresent(failed, ignored);
        }
        return false;
    }

    if (!removeIfPresent(rollback, error)) return false;
    return commitSd(error);
}

bool writeTextAtomic(
    const std::string& target,
    const std::string& text,
    std::string& error) {
    const std::string staged = target + ".nxsync-new";
    FILE* file = std::fopen(staged.c_str(), "wb");
    if (file == nullptr) {
        error = errnoMessage("Failed to open marker");
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), file)
        == text.size();
    const bool flushed = written && std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    std::fclose(file);
    if (!synced) {
        error = errnoMessage("Failed to write marker");
        return false;
    }
    if (!commitSd(error)) return false;
    if (!removeIfPresent(target, error)
        || !renameFile(staged, target, error)) {
        return false;
    }
    return commitSd(error);
}

std::string backupPathFor(const nxsync::installer::PayloadDescriptor& payload) {
    return std::string("sdmc:/atmosphere/stratosphere.romfs.nxsync-original-")
        + payload.package3Sha256;
}

std::string payloadSourcePath(
    const nxsync::installer::PayloadDescriptor& payload) {
    return "romfs:/" + payload.dmntPath;
}

bool validatePayloadFiles(
    const nxsync::installer::PayloadManifest& manifest,
    const nxsync::installer::PayloadDescriptor& payload,
    std::string& error) {
    const FileHash dmnt = hashFile(payloadSourcePath(payload));
    if (!dmnt.ok || dmnt.sha256 != payload.dmntOverrideSha256) {
        error = dmnt.ok
            ? "Corrupted dmnt.nsp payload"
            : dmnt.error;
        return false;
    }
    if (manifest.commonPayloads.size() != std::size(CommonPayloads)) {
        error = "Unexpected common payload count";
        return false;
    }
    for (const auto& common : CommonPayloads) {
        const std::string sourcePath = common.source;
        constexpr const char* RomfsPrefix = "romfs:/";
        if (sourcePath.rfind(RomfsPrefix, 0) != 0) {
            error = "Invalid internal common payload path";
            return false;
        }
        const std::string manifestPath = sourcePath.substr(
            std::strlen(RomfsPrefix));
        const auto descriptor = std::find_if(
            manifest.commonPayloads.begin(),
            manifest.commonPayloads.end(),
            [&](const nxsync::installer::CommonPayloadDescriptor& candidate) {
                return candidate.path == manifestPath;
            });
        if (descriptor == manifest.commonPayloads.end()) {
            error = "Undeclared common payload: " + manifestPath;
            return false;
        }
        const FileHash source = hashFile(common.source);
        if (!source.ok || source.sha256 != descriptor->sha256) {
            error = source.ok
                ? "Common payload hash mismatch: " + manifestPath
                : "Missing common payload: " + std::string(common.source);
            return false;
        }
    }
    return true;
}

bool removeRecognizedArtifact(
    const std::string& path,
    const std::vector<std::string>& allowedHashes,
    std::string& error) {
    if (!fileExists(path)) return true;

    const FileHash artifact = hashFile(path);
    if (!artifact.ok) {
        error = artifact.error;
        return false;
    }
    if (std::find(
            allowedHashes.begin(),
            allowedHashes.end(),
            artifact.sha256) == allowedHashes.end()) {
        error = "Unrecognized previous NXSync artifact: " + path;
        return false;
    }
    return removeIfPresent(path, error);
}

bool cleanupIntegratedInstallerArtifacts(
    const nxsync::installer::PayloadDescriptor& payload,
    std::string& error) {
    const std::vector<std::string> official{payload.officialRomfsSha256};
    const std::vector<std::string> integrated{payload.integratedRomfsSha256};
    if (!removeRecognizedArtifact(backupPathFor(payload), official, error)
        || !removeRecognizedArtifact(
            std::string(StratosphereRomfsPath) + ".nxsync-new",
            integrated,
            error)
        || !removeRecognizedArtifact(
            std::string(StratosphereRomfsPath) + ".nxsync-rollback",
            official,
            error)
        || !removeRecognizedArtifact(
            std::string(StratosphereRomfsPath) + ".nxsync-failed",
            integrated,
            error)) {
        return false;
    }
    return commitSd(error);
}

bool installCommonPayloads(std::string& error) {
    if (!nxsync::installer::ensureInstallDirectories("sdmc:", error)) return false;
    for (const auto& common : CommonPayloads) {
        if (common.preserveExisting && fileExists(common.target)) continue;

        const FileHash source = hashFile(common.source);
        if (!source.ok) {
            error = source.error;
            return false;
        }
        if (!atomicReplaceFromFile(
                common.source,
                common.target,
                source.sha256,
                error)) {
            return false;
        }
    }
    return true;
}

bool installNxsync(
    const nxsync::installer::PayloadManifest& manifest,
    const nxsync::installer::PayloadDescriptor& payload,
    const nxsync::installer::CompatibilityState state,
    std::string& error) {
    if (!validatePayloadFiles(manifest, payload, error)) return false;
    if (state != nxsync::installer::CompatibilityState::ReadyToInstall
        && state != nxsync::installer::CompatibilityState::UpgradeAvailable
        && state != nxsync::installer::CompatibilityState::InstalledCurrent) {
        error = "Unsupported environment: installation refused";
        return false;
    }

    if (!nxsync::installer::ensureInstallDirectories("sdmc:", error)) return false;
    if (!cleanupIntegratedInstallerArtifacts(payload, error)) return false;
    if (state != nxsync::installer::CompatibilityState::InstalledCurrent) {
        if (!atomicReplaceFromFile(
            payloadSourcePath(payload),
            DmntOverridePath,
            payload.dmntOverrideSha256,
            error)) {
            return false;
        }
    }
    if (!installCommonPayloads(error)) return false;

    const std::string marker =
        "schema=nxsync-installer-lock\n"
        "version=2\n"
        "installer_version=" + std::string(InstallerVersion) + "\n"
        "payload_id=" + payload.id + "\n"
        "atmosphere_version=" + payload.atmosphereVersion + "\n"
        "package3_sha256=" + payload.package3Sha256 + "\n"
        "stratosphere_romfs_sha256=" + payload.officialRomfsSha256 + "\n"
        "dmnt_override_sha256=" + payload.dmntOverrideSha256 + "\n";
    return writeTextAtomic(InstallerMarkerPath, marker, error);
}

bool removeExactInstalledFile(
    const CommonPayload& common,
    std::string& error) {
    if (common.preserveExisting || !fileExists(common.target)) return true;

    const FileHash source = hashFile(common.source);
    const FileHash target = hashFile(common.target);
    if (!source.ok || !target.ok) {
        error = !source.ok ? source.error : target.error;
        return false;
    }
    if (source.sha256 != target.sha256) return true;
    return removeIfPresent(common.target, error);
}

bool uninstallNxsync(
    const nxsync::installer::PayloadDescriptor& payload,
    const nxsync::installer::CompatibilityState state,
    std::string& error) {
    if (state != nxsync::installer::CompatibilityState::InstalledCurrent
        && state != nxsync::installer::CompatibilityState::UpgradeAvailable
        && state != nxsync::installer::CompatibilityState::ReadyToInstall) {
        error = "Unsupported environment: uninstall refused";
        return false;
    }

    if (!cleanupIntegratedInstallerArtifacts(payload, error)) return false;
    if (state != nxsync::installer::CompatibilityState::ReadyToInstall
        && !removeIfPresent(DmntOverridePath, error)) {
        return false;
    }

    for (const auto& common : CommonPayloads) {
        if (!removeExactInstalledFile(common, error)) return false;
    }
    if (!removeIfPresent(InstallerMarkerPath, error)) return false;
    return commitSd(error);
}

bool loadManifest(
    nxsync::installer::PayloadManifest& manifest,
    std::string& error) {
    std::string text;
    return readTextFile(ManifestPath, text, error)
        && nxsync::installer::parsePayloadManifest(text, manifest, error);
}

Detection detect(
    const nxsync::installer::PayloadManifest& manifest) {
    Detection detection;
    detection.overlayDependencies = nxsync::detectOverlayDependencies();
    detection.package3 = hashFile(Package3Path);
    detection.stratosphereRomfs = hashFile(StratosphereRomfsPath);
    if (!detection.package3.ok || !detection.stratosphereRomfs.ok) {
        detection.compatibility.message = !detection.package3.ok
            ? detection.package3.error
            : detection.stratosphereRomfs.error;
        return detection;
    }

    if (fileExists(DmntOverridePath)) {
        detection.dmntOverridePresent = true;
        detection.dmntOverride = hashFile(DmntOverridePath);
        if (!detection.dmntOverride.ok) {
            detection.compatibility.message = detection.dmntOverride.error;
            return detection;
        }
    }

    detection.compatibility = nxsync::installer::classifyInstallation(
        manifest.payloads,
        detection.package3.sha256,
        detection.stratosphereRomfs.sha256,
        detection.dmntOverridePresent ? detection.dmntOverride.sha256 : "");
    if (detection.compatibility.supported()) {
        std::string error;
        detection.payloadValid = validatePayloadFiles(
            manifest,
            manifest.payloads[detection.compatibility.payloadIndex],
            error);
        detection.payloadError = error;
    }
    return detection;
}

std::string shortHash(const FileHash& hash) {
    if (!hash.ok || hash.sha256.size() < 16) return "unavailable";
    return hash.sha256.substr(0, 16) + "...";
}

void render(
    const Detection& detection,
    const std::vector<nxsync::installer::PayloadDescriptor>& payloads,
    const bool installConfirmation,
    const bool uninstallConfirmation,
    const std::string& operationMessage,
    const bool operationOk) {
    consoleClear();
    std::printf("NXSync Installer %s\n", InstallerVersion);
    std::printf("===============================\n\n");
    std::printf("package3:             %s\n", shortHash(detection.package3).c_str());
    std::printf("stratosphere.romfs:   %s\n\n",
        shortHash(detection.stratosphereRomfs).c_str());
    std::printf("dmnt override:        %s\n\n",
        detection.dmntOverridePresent
            ? shortHash(detection.dmntOverride).c_str()
            : "not present");
    std::printf("Overlay dependencies\n");
    std::printf("nx-ovlloader:         %s\n",
        detection.overlayDependencies.loaderBinary
            ? (detection.overlayDependencies.loaderBootFlag
                ? "detected and boot-enabled"
                : "detected; boot flag missing")
            : "missing");
    std::printf("Overlay menu:         %s\n",
        detection.overlayDependencies.overlayMenu ? "detected" : "missing");
    std::printf("Automatic preflight:  %s\n\n",
        detection.overlayDependencies.ready()
            ? "dependency files detected"
            : "blocked until dependencies are installed");
    std::printf("Status: %s\n",
        nxsync::installer::compatibilityStateName(
            detection.compatibility.state));
    std::printf("%s\n", detection.compatibility.message.c_str());

    if (detection.compatibility.payloadIndex < payloads.size()) {
        const auto& payload = payloads[detection.compatibility.payloadIndex];
        std::printf("Supported Atmosphere: %s\n", payload.atmosphereVersion.c_str());
        if (detection.compatibility.supported() && !detection.payloadValid) {
            std::printf("Invalid installer payload: %s\n",
                detection.payloadError.c_str());
        }
    }

    if (!operationMessage.empty()) {
        std::printf("\n%s: %s\n",
            operationOk ? "OK" : "ERROR",
            operationMessage.c_str());
    }

    std::printf("\n");
    if (installConfirmation) {
        std::printf("Confirm installation with ZL + ZR + A.\n");
        std::printf("B cancels.\n");
    } else if (uninstallConfirmation) {
        std::printf("Confirm uninstall with ZL + ZR + X.\n");
        std::printf("B cancels. Configuration and backups will be preserved.\n");
    } else if (detection.compatibility.supported()
        && detection.payloadValid) {
        std::printf("A  Install/update NXSync components\n");
        std::printf("X  Remove recognized NXSync components\n");
    } else {
        std::printf("Installation blocked: no compatible build.\n");
    }
    std::printf("Y  Refresh detection\n");
    std::printf("+  Exit\n");
    consoleUpdate(nullptr);
}

} // namespace

int main(int, char**) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    const Result romfsResult = romfsInit();
    nxsync::installer::PayloadManifest manifest;
    std::string startupError;
    if (R_FAILED(romfsResult)) {
        startupError = "Unable to mount the installer RomFS";
    } else if (!loadManifest(manifest, startupError)) {
        // Keep the error and show a non-writing diagnostic screen.
    }

    Detection detection;
    if (startupError.empty()) {
        std::printf("NXSync Installer %s\n\nCalculating SHA-256...\n",
            InstallerVersion);
        consoleUpdate(nullptr);
        detection = detect(manifest);
    } else {
        detection.compatibility.message = startupError;
    }

    bool installConfirmation = false;
    bool uninstallConfirmation = false;
    std::string operationMessage;
    bool operationOk = false;
    bool redraw = true;

    while (appletMainLoop()) {
        if (redraw) {
            render(
                detection,
                manifest.payloads,
                installConfirmation,
                uninstallConfirmation,
                operationMessage,
                operationOk);
            redraw = false;
        }

        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 held = padGetButtons(&pad);
        if ((down & HidNpadButton_Plus) != 0) break;

        if ((down & HidNpadButton_B) != 0) {
            installConfirmation = false;
            uninstallConfirmation = false;
            redraw = true;
            continue;
        }

        if ((down & HidNpadButton_Y) != 0 && startupError.empty()) {
            std::printf("\nRefreshing SHA-256...\n");
            consoleUpdate(nullptr);
            detection = detect(manifest);
            operationMessage.clear();
            installConfirmation = false;
            uninstallConfirmation = false;
            redraw = true;
            continue;
        }

        const bool canOperate = detection.compatibility.supported()
            && detection.payloadValid
            && detection.compatibility.payloadIndex < manifest.payloads.size();
        if (!canOperate) continue;

        if (!installConfirmation && !uninstallConfirmation
            && (down & HidNpadButton_A) != 0) {
            installConfirmation = true;
            operationMessage.clear();
            redraw = true;
            continue;
        }
        if (!installConfirmation && !uninstallConfirmation
            && (down & HidNpadButton_X) != 0) {
            uninstallConfirmation = true;
            operationMessage.clear();
            redraw = true;
            continue;
        }

        if (installConfirmation
            && (down & HidNpadButton_A) != 0
            && (held & HidNpadButton_ZL) != 0
            && (held & HidNpadButton_ZR) != 0) {
            const auto state = detection.compatibility.state;
            std::printf("\nInstallation in progress. Do not turn off the console...\n");
            consoleUpdate(nullptr);
            std::string error;
            operationOk = installNxsync(
                manifest,
                manifest.payloads[detection.compatibility.payloadIndex],
                state,
                error);
            operationMessage = operationOk
                ? "Installation verified. Fully restart Atmosphere"
                : error;
            installConfirmation = false;
            detection = detect(manifest);
            redraw = true;
            continue;
        }

        if (uninstallConfirmation
            && (down & HidNpadButton_X) != 0
            && (held & HidNpadButton_ZL) != 0
            && (held & HidNpadButton_ZR) != 0) {
            const auto state = detection.compatibility.state;
            std::printf("\nRemoving NXSync. Do not turn off the console...\n");
            consoleUpdate(nullptr);
            std::string error;
            operationOk = uninstallNxsync(
                manifest.payloads[detection.compatibility.payloadIndex],
                state,
                error);
            operationMessage = operationOk
                ? "NXSync components removed. Fully restart Atmosphere"
                : error;
            uninstallConfirmation = false;
            detection = detect(manifest);
            redraw = true;
        }
    }

    if (R_SUCCEEDED(romfsResult)) romfsExit();
    consoleExit(nullptr);
    return 0;
}
