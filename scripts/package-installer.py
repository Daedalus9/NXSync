#!/usr/bin/env python3
"""Create the user-facing NXSync installer archive and checksum manifest."""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import zipfile
from pathlib import Path
from installer_romfs import verify_staged_romfs
from source_release import create_source_release


ROOT = Path(__file__).resolve().parent.parent
NRO_HEADER_OFFSET = 0x10
NRO_SIZE_OFFSET = NRO_HEADER_OFFSET + 0x08
NRO_ASSET_HEADER = struct.Struct("<IIQQQQQQ")
NRO_MAGIC = b"NRO0"
NRO_ASSET_MAGIC = 0x54455341


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require_embedded_romfs(path: Path) -> None:
    """Reject installer NROs that do not contain an embedded RomFS."""
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(NRO_SIZE_OFFSET + 4)
        if len(header) < NRO_SIZE_OFFSET + 4:
            raise RuntimeError("Installer NRO header is truncated")
        if header[NRO_HEADER_OFFSET:NRO_HEADER_OFFSET + 4] != NRO_MAGIC:
            raise RuntimeError("Installer build is not a valid NRO")

        nro_size = struct.unpack_from("<I", header, NRO_SIZE_OFFSET)[0]
        if nro_size + NRO_ASSET_HEADER.size > file_size:
            raise RuntimeError("Installer NRO has no asset header")

        stream.seek(nro_size)
        asset = stream.read(NRO_ASSET_HEADER.size)
        (
            asset_magic,
            _asset_version,
            _icon_offset,
            _icon_size,
            _nacp_offset,
            _nacp_size,
            romfs_offset,
            romfs_size,
        ) = NRO_ASSET_HEADER.unpack(asset)
        if asset_magic != NRO_ASSET_MAGIC:
            raise RuntimeError("Installer NRO has no valid asset header")
        if romfs_size == 0:
            raise RuntimeError("Installer NRO has no embedded RomFS")
        romfs_start = nro_size + romfs_offset
        romfs_end = romfs_start + romfs_size
        if romfs_offset < NRO_ASSET_HEADER.size or romfs_end > file_size:
            raise RuntimeError("Installer NRO has an invalid embedded RomFS range")
        stream.seek(romfs_start)
        verify_staged_romfs(stream.read(romfs_size), ROOT / "installer" / "romfs")


def main() -> int:
    versions = json.loads((ROOT / "versions.json").read_text(encoding="utf-8"))
    version = versions["installer"]
    installer = ROOT / "installer" / "NXSyncInstaller.nro"
    generated = ROOT / "installer" / "payload-manifest.generated.json"
    if not installer.is_file():
        raise RuntimeError(f"Missing installer build: {installer}")
    require_embedded_romfs(installer)
    if not generated.is_file():
        raise RuntimeError("Installer payloads were not staged and verified")

    payload_manifest = json.loads(generated.read_text(encoding="utf-8"))
    if (
        payload_manifest.get("schema") != "nxsync-installer-staging"
        or payload_manifest.get("version") != 2
    ):
        raise RuntimeError("Invalid generated installer payload manifest")
    atmosphere_license = ROOT / ".artifacts" / "atmosphere" / "1.11.2" / "LICENSE"
    expected_license = payload_manifest.get("atmosphere_license_sha256")
    if (
        not atmosphere_license.is_file()
        or not isinstance(expected_license, str)
        or sha256(atmosphere_license) != expected_license
    ):
        raise RuntimeError("Verified Atmosphere license file is unavailable")

    archive_name = f"NXSync-Installer-{version}.zip"
    destination = ROOT / "dist" / archive_name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise RuntimeError(f"Release archive already exists: {destination}")

    # Keep the installer in its own hbmenu application directory. Placing two
    # NROs below switch/NXSync can make hbmenu expose only NXSync.nro.
    target = "switch/NXSyncInstaller/NXSyncInstaller.nro"
    packaged_files = {
        target: installer,
        "licenses/Atmosphere-GPL-2.0.txt": atmosphere_license,
        "licenses/NXSync-MIT.txt": ROOT / "LICENSE",
        "licenses/NXSync-LICENSE-MAP.md": ROOT / "LICENSES" / "README.md",
        "licenses/libultrahand-GPL-2.0.txt": ROOT
        / "overlay"
        / "lib"
        / "libultrahand"
        / "LICENSE",
        "licenses/libultrahand-CC-BY-4.0.txt": ROOT
        / "overlay"
        / "lib"
        / "libultrahand"
        / "SUB_LICENSE",
        "licenses/libtesla-GPL-2.0.txt": ROOT
        / "overlay"
        / "lib"
        / "libultrahand"
        / "libtesla"
        / "LICENSE",
        "licenses/libultra-GPL-2.0.txt": ROOT
        / "overlay"
        / "lib"
        / "libultrahand"
        / "libultra"
        / "LICENSE",
        "licenses/libultra-CC-BY-4.0.txt": ROOT
        / "overlay"
        / "lib"
        / "libultrahand"
        / "libultra"
        / "SUB_LICENSE",
        "licenses/cJSON-MIT.txt": ROOT
        / "LICENSES"
        / "third-party"
        / "cJSON-MIT.txt",
        "licenses/stb-MIT.txt": ROOT
        / "LICENSES"
        / "third-party"
        / "stb-MIT.txt",
        "licenses/OVERLAY-COMPONENTS.md": ROOT
        / "overlay"
        / "lib"
        / "libultrahand"
        / "COMPONENTS.md",
        "THIRD_PARTY_NOTICES.md": ROOT / "THIRD_PARTY_NOTICES.md",
    }
    dependency_license_directory = (
        ROOT / ".artifacts" / "dependency-licenses"
    )
    required_dependency_licenses = {
        "QR-Code-generator-MIT.txt",
        "GCC-GPL-3.0.txt",
        "GCC-Runtime-Exception-3.1.txt",
        "newlib-4.6.0-COPYING.txt",
        "newlib-4.4.0-COPYING.txt",
        "bzip2-LICENSE.txt",
        "curl-COPYING.txt",
        "FreeType-FTL.txt",
        "FreeType-GPL-2.0.txt",
        "FreeType-LICENSE.txt",
        "HarfBuzz-COPYING.txt",
        "libdrm-COPYING.txt",
        "libnx-ISC.txt",
        "libpng-LICENSE.txt",
        "MbedTLS-LICENSE.txt",
        "Mesa-MIT.txt",
        "MiniZip-Zlib.txt",
        "SDL2-Zlib.txt",
        "SDL2_ttf-Zlib.txt",
        "zlib-LICENSE.txt",
        "SHA256SUMS.txt",
    }
    available_dependency_licenses = {
        path.name: path
        for path in dependency_license_directory.glob("*")
        if path.is_file()
    }
    missing_dependency_licenses = sorted(
        required_dependency_licenses - available_dependency_licenses.keys()
    )
    if missing_dependency_licenses:
        raise RuntimeError(
            "Staged dependency licenses are unavailable: "
            + ", ".join(missing_dependency_licenses)
        )
    licensed_names = set()
    for line in available_dependency_licenses["SHA256SUMS.txt"].read_text().splitlines():
        digest, name = line.split("  ", 1)
        if name not in available_dependency_licenses or sha256(available_dependency_licenses[name]) != digest:
            raise RuntimeError("Staged license checksum mismatch: " + name)
        licensed_names.add(name)
    if licensed_names != available_dependency_licenses.keys() - {"SHA256SUMS.txt"}:
        raise RuntimeError("Incomplete staged license checksum inventory")
    for name, path in sorted(available_dependency_licenses.items()):
        packaged_files[f"licenses/dependencies/{name}"] = path
    missing_notices = [
        str(path) for path in packaged_files.values() if not path.is_file()
    ]
    if missing_notices:
        raise RuntimeError(
            "Required license/notice files are unavailable: "
            + ", ".join(missing_notices)
        )
    compatibility = json.loads(
        (ROOT / "compatibility.json").read_text(encoding="utf-8")
    )
    generated_by_version = {
        build["atmosphere_version"]: build
        for build in payload_manifest.get("builds", [])
    }
    source_assets: list[tuple[Path, str, str]] = []
    source_assets_by_version: dict[str, tuple[str, str]] = {}
    for build in compatibility["builds"]:
        version_name = build["atmosphere_version"]
        published_hash = build.get("nxsync_dmnt_override_sha256")
        generated_build = generated_by_version.get(version_name, {})
        if (
            not isinstance(published_hash, str)
            or len(published_hash) != 64
            or generated_build.get("nxsync_dmnt_override_sha256")
            != published_hash
        ):
            raise RuntimeError(
                f"Atmosphere {version_name} has no recorded dmnt override hash"
            )
        source_name = generated_build.get("corresponding_source_file")
        source_hash = generated_build.get("corresponding_source_sha256")
        if (
            not isinstance(source_name, str)
            or Path(source_name).name != source_name
            or not source_name.endswith(".tar.xz")
            or not isinstance(source_hash, str)
            or len(source_hash) != 64
        ):
            raise RuntimeError(
                f"Atmosphere {version_name} has no corresponding-source record"
            )
        source_path = (
            ROOT / ".artifacts" / "atmosphere" / version_name / source_name
        )
        if not source_path.is_file() or sha256(source_path) != source_hash:
            raise RuntimeError(
                f"Atmosphere {version_name} corresponding source is unavailable"
            )
        release_source_name = (
            f"NXSync-Installer-{version}-Atmosphere-{version_name}-"
            "corresponding-source.tar.xz"
        )
        release_source_path = destination.parent / release_source_name
        if release_source_path.exists():
            raise RuntimeError(
                f"Release source archive already exists: {release_source_path}"
            )
        source_assets.append((source_path, release_source_name, source_hash))
        source_assets_by_version[version_name] = (
            release_source_name,
            source_hash,
        )
    nxsync_source_name = f"NXSync-Installer-{version}-NXSync-corresponding-source.tar.xz"
    nxsync_source, nxsync_source_hash = create_source_release(
        ROOT, destination.parent / nxsync_source_name)
    source_assets.append((nxsync_source, nxsync_source_name, nxsync_source_hash))
    source_notice = [
        "Atmosphere source corresponding to the embedded modified binaries",
        "=================================================================",
        "",
        "Upstream: https://github.com/Atmosphere-NX/Atmosphere",
        "",
    ]
    for build in compatibility["builds"]:
        source_notice.extend(
            [
                f"Atmosphere {build['atmosphere_version']}",
                f"  commit: {build['upstream_commit']}",
                f"  official release: {build['official_release_url']}",
                f"  official ZIP SHA-256: {build['official_release_zip_sha256']}",
                f"  official package3 SHA-256: {build['official_package3_sha256']}",
                (
                    "  official stratosphere.romfs SHA-256: "
                    f"{build['official_stratosphere_romfs_sha256']}"
                ),
                (
                    "  NXSync dmnt override SHA-256: "
                    f"{build['nxsync_dmnt_override_sha256']}"
                ),
                (
                    "  integrated ROMFS build-evidence SHA-256: "
                    f"{build['nxsync_stratosphere_romfs_sha256']}"
                ),
                f"  NXSync patch: {build['dmnt_patch']}",
                (
                    "  complete corresponding source: "
                    f"{source_assets_by_version[str(build['atmosphere_version'])][0]}"
                ),
                (
                    "  corresponding source SHA-256: "
                    f"{source_assets_by_version[str(build['atmosphere_version'])][1]}"
                ),
            ]
        )
    source_notice.extend(
        [
            "",
            "The NXSync repository contains the complete patch and reproducible",
            "build scripts used for these payloads. The complete patched source",
            "archives listed above are sibling assets of this installer release.",
            "",
        ]
    )
    readme = (
        "NXSync Installer\n"
        "================\n\n"
        f"Installer build: {version}\n\n"
        "Before installing or updating, copy atmosphere/ and config/ from the SD "
        "card to a dated backup on a PC or external drive. Also copy switch/NXSync/ "
        "and switch/.overlays/ if present. Fully power off the console before "
        "removing the SD card. Verify the copy and record the Atmosphere version; "
        "do not mix files from different Atmosphere versions during recovery. "
        "Keep an independent copy of important game saves as well.\n\n"
        "Copy the switch directory to the root of the SD card, launch "
        "NXSyncInstaller.nro through full-memory homebrew mode, and follow the "
        "on-screen confirmation. The installer refuses unknown Atmosphere "
        "package3/stratosphere.romfs/dmnt combinations. It never rewrites the "
        "active package3 or stratosphere.romfs. Cold-boot Atmosphere after "
        "installation. See the project documentation before updating or "
        "removing Atmosphere.\n"
    )
    generated_files = {
        "README-INSTALL.txt": readme.encode("utf-8"),
        "NXSYNC-SOURCE.txt": (
            "Complete NXSync and overlay source, including linked dependency sources,\n"
            "patches and build recipes, is provided in the sibling release asset:\n"
            + nxsync_source_name + "\nSHA-256: " + nxsync_source_hash + "\n"
        ).encode("utf-8"),
        "ATMOSPHERE-SOURCE.txt": "\n".join(source_notice).encode("utf-8"),
        "CORRESPONDING-SOURCE-SHA256SUMS.txt": "".join(
            f"{digest}  {name}\n" for _, name, digest in source_assets
        ).encode("ascii"),
    }
    checksums = "".join(
        [
            f"{sha256(path)}  {archive_path}\n"
            for archive_path, path in sorted(packaged_files.items())
        ]
        + [
            f"{sha256_bytes(data)}  {archive_path}\n"
            for archive_path, data in sorted(generated_files.items())
        ]
    )

    with zipfile.ZipFile(
        destination, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for archive_path, source in packaged_files.items():
            archive.write(source, archive_path)
        for archive_path, data in generated_files.items():
            archive.writestr(archive_path, data)
        archive.writestr("SHA256SUMS.txt", checksums)

    with zipfile.ZipFile(destination) as archive:
        bad = archive.testzip()
        if bad is not None:
            raise RuntimeError(f"Archive integrity failed at {bad}")
        if hashlib.sha256(archive.read(target)).hexdigest() != sha256(installer):
            raise RuntimeError("Packaged installer hash mismatch")

    for source_path, release_name, source_hash in source_assets:
        release_path = destination.parent / release_name
        if source_path.resolve() == release_path.resolve():
            continue
        temporary = release_path.with_suffix(release_path.suffix + ".part")
        if temporary.exists():
            temporary.unlink()
        shutil.copy2(source_path, temporary)
        if sha256(temporary) != source_hash:
            temporary.unlink(missing_ok=True)
            raise RuntimeError(
                f"Copied corresponding source mismatch: {release_name}"
            )
        temporary.replace(release_path)

    release_files = [destination] + [destination.parent / name for _, name, _ in source_assets]
    (destination.parent / "SHA256SUMS.txt").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(release_files)), encoding="ascii")
    print(destination)
    print(f"SHA-256: {sha256(destination)}")
    for _, release_name, source_hash in source_assets:
        print(destination.parent / release_name)
        print(f"SHA-256: {source_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
