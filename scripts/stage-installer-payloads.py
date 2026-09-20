#!/usr/bin/env python3
"""Stage verified common and Atmosphere payloads into the installer RomFS."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_verified(source: Path, target: Path, expected: str | None = None) -> str:
    if not source.is_file():
        raise RuntimeError(f"Missing build artifact: {source}")
    actual = sha256(source)
    if expected and actual != expected.lower():
        raise RuntimeError(f"Hash mismatch for {source}: {actual} != {expected}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    copied = sha256(target)
    if copied != actual:
        raise RuntimeError(f"Copy verification failed: {target}")
    return actual


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--atmosphere-artifacts",
        type=Path,
        default=ROOT / ".artifacts" / "atmosphere",
    )
    args = parser.parse_args()

    compatibility = json.loads(
        (ROOT / "compatibility.json").read_text(encoding="utf-8")
    )
    if (
        compatibility.get("schema") != "nxsync-atmosphere-compatibility"
        or compatibility.get("schema_version") != 2
    ):
        raise RuntimeError("Unsupported compatibility.json schema")
    romfs_root = ROOT / "installer" / "romfs"
    payload_root = romfs_root / "payloads"
    common_root = romfs_root / "common"
    payload_root.mkdir(parents=True, exist_ok=True)

    if common_root.exists():
        shutil.rmtree(common_root)
    for child in payload_root.iterdir():
        if child.name != ".gitkeep":
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()

    common_files = {
        ROOT / "NXSync.nro": common_root / "switch/NXSync/NXSync.nro",
        ROOT / "overlay/NXSync.ovl": common_root / "switch/.overlays/NXSync.ovl",
        ROOT / "sysmodule/nxsync-sysmodule.nsp": (
            common_root
            / "atmosphere/contents/4200000000004E58/exefs.nsp"
        ),
        ROOT / "cloud-worker/nxsync-cloud-worker.nsp": (
            common_root
            / "atmosphere/contents/4200000000004E59/exefs.nsp"
        ),
        ROOT / "sysmodule/nxsync-sysmodule.ini": (
            common_root / "config/NXSync/sysmodule.ini"
        ),
    }
    common_hashes: dict[str, str] = {}
    for source, target in common_files.items():
        common_hashes[str(target.relative_to(common_root)).replace("\\", "/")] = (
            copy_verified(source, target)
        )

    flag = common_root / "atmosphere/contents/4200000000004E58/flags/boot2.flag"
    flag.parent.mkdir(parents=True, exist_ok=True)
    flag.write_bytes(b"")
    common_hashes[str(flag.relative_to(common_root)).replace("\\", "/")] = sha256(flag)

    lines = [
        "schema=nxsync-installer-payloads",
        "version=2",
        f"common.count={len(common_hashes)}",
    ]
    for index, (relative, digest) in enumerate(sorted(common_hashes.items())):
        lines.extend(
            [
                f"common.{index}.path=common/{relative}",
                f"common.{index}.sha256={digest}",
            ]
        )
    lines.extend(
        [
        f"count={len(compatibility['builds'])}",
        ]
    )
    generated_builds: list[dict[str, object]] = []
    atmosphere_license_hash: str | None = None
    artifacts = args.atmosphere_artifacts.resolve()

    for index, build in enumerate(compatibility["builds"]):
        version = build["atmosphere_version"]
        metadata_path = artifacts / version / "payload.json"
        source_romfs = artifacts / version / "stratosphere.romfs"
        source_dmnt = artifacts / version / "dmnt.nsp"
        source_license = artifacts / version / "LICENSE"
        if not metadata_path.is_file():
            raise RuntimeError(f"Missing Atmosphere payload metadata: {metadata_path}")
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if (
            metadata.get("schema") != "nxsync-atmosphere-payload"
            or metadata.get("version") != 3
            or metadata.get("atmosphere_version") != version
        ):
            raise RuntimeError(f"Invalid payload metadata for Atmosphere {version}")
        if metadata["upstream_commit"] != build["upstream_commit"]:
            raise RuntimeError(f"Commit mismatch for Atmosphere {version}")
        if metadata.get("upstream_branch") != build["upstream_branch"]:
            raise RuntimeError(f"Branch metadata mismatch for Atmosphere {version}")
        if metadata.get("libnx_commit") != build.get("libnx_commit"):
            raise RuntimeError(f"libnx metadata mismatch for Atmosphere {version}")
        if metadata.get("toolchain_image") != build.get("toolchain_image"):
            raise RuntimeError(f"Pinned toolchain mismatch for Atmosphere {version}")
        if metadata["package3_sha256"] != build["official_package3_sha256"]:
            raise RuntimeError(f"package3 mismatch for Atmosphere {version}")
        if (
            metadata.get("official_release_zip_sha256")
            != build["official_release_zip_sha256"]
        ):
            raise RuntimeError(f"Official release mismatch for Atmosphere {version}")
        if (
            metadata.get("official_stratosphere_romfs_sha256")
            != build["official_stratosphere_romfs_sha256"]
            or metadata.get("official_romfs_roundtrip_verified") is not True
        ):
            raise RuntimeError(f"Official ROMFS base mismatch for Atmosphere {version}")
        if metadata.get("only_dmnt_changed_verified") is not True:
            raise RuntimeError(
                f"ROMFS file-diff verification is missing for Atmosphere {version}"
            )
        patched_dmnt_hash = metadata.get("patched_dmnt_sha256")
        if not isinstance(patched_dmnt_hash, str) or len(patched_dmnt_hash) != 64:
            raise RuntimeError(f"Missing patched dmnt hash for Atmosphere {version}")
        if patched_dmnt_hash != build.get("nxsync_dmnt_override_sha256"):
            raise RuntimeError(
                f"Published dmnt hash mismatch for Atmosphere {version}: "
                f"built {patched_dmnt_hash}, "
                f"expected {build.get('nxsync_dmnt_override_sha256')}"
            )

        expected_romfs = build.get("nxsync_stratosphere_romfs_sha256") or None
        metadata_romfs = metadata.get("stratosphere_romfs_sha256")
        if not isinstance(metadata_romfs, str) or len(metadata_romfs) != 64:
            raise RuntimeError(f"Missing ROMFS hash for Atmosphere {version}")
        if expected_romfs and metadata_romfs != expected_romfs:
            raise RuntimeError(
                f"Published ROMFS hash mismatch for Atmosphere {version}"
            )
        license_hash = metadata.get("license_sha256")
        if not isinstance(license_hash, str) or len(license_hash) != 64:
            raise RuntimeError(f"Missing license hash for Atmosphere {version}")
        if not source_license.is_file() or sha256(source_license) != license_hash:
            raise RuntimeError(f"Atmosphere license hash mismatch for {version}")
        if atmosphere_license_hash is None:
            atmosphere_license_hash = license_hash
        elif atmosphere_license_hash != license_hash:
            raise RuntimeError("Atmosphere variants contain different license texts")
        if not source_romfs.is_file() or sha256(source_romfs) != metadata_romfs:
            raise RuntimeError(f"Integrated ROMFS evidence mismatch for {version}")
        source_archive_name = metadata.get("corresponding_source_file")
        source_archive_hash = metadata.get("corresponding_source_sha256")
        if (
            not isinstance(source_archive_name, str)
            or not source_archive_name.endswith(".tar.xz")
            or Path(source_archive_name).name != source_archive_name
            or not isinstance(source_archive_hash, str)
            or len(source_archive_hash) != 64
        ):
            raise RuntimeError(
                f"Missing corresponding-source metadata for Atmosphere {version}"
            )
        source_archive = artifacts / version / source_archive_name
        if (
            not source_archive.is_file()
            or sha256(source_archive) != source_archive_hash
        ):
            raise RuntimeError(
                f"Corresponding-source archive mismatch for Atmosphere {version}"
            )
        if metadata.get("corresponding_source_atmosphere_files", 0) < 1:
            raise RuntimeError(
                f"Atmosphere source tree is empty for {version}"
            )
        with tarfile.open(source_archive, "r:xz") as source_bundle:
            patches = [member for member in source_bundle.getmembers()
                       if member.name.endswith("/nxsync-build-inputs/dmnt-cheat-api.patch")]
            if len(patches) != 1 or not patches[0].isfile():
                raise RuntimeError(f"Missing exact source patch for Atmosphere {version}")
            patch_bytes = source_bundle.extractfile(patches[0]).read()
            if patch_bytes.replace(b"\r\n", b"\n") != (ROOT / build["dmnt_patch"]).read_bytes().replace(b"\r\n", b"\n"):
                raise RuntimeError(f"Stale source patch for Atmosphere {version}")
        if build.get("libnx_commit") and metadata.get(
            "corresponding_source_libnx_files", 0
        ) < 1:
            raise RuntimeError(f"libnx source tree is empty for {version}")
        target_relative = f"payloads/{version}/dmnt.nsp"
        target_dmnt = romfs_root / target_relative
        copied_dmnt_hash = copy_verified(
            source_dmnt, target_dmnt, patched_dmnt_hash
        )

        previous = build.get("previous_nxsync_dmnt_override_sha256", [])
        lines.extend(
            [
                f"payload.{index}.id=atmosphere-{version}",
                f"payload.{index}.atmosphere_version={version}",
                f"payload.{index}.package3_sha256={build['official_package3_sha256']}",
                (
                    f"payload.{index}.official_romfs_sha256="
                    f"{build['official_stratosphere_romfs_sha256']}"
                ),
                (
                    f"payload.{index}.integrated_romfs_sha256="
                    f"{build['nxsync_stratosphere_romfs_sha256']}"
                ),
                f"payload.{index}.dmnt_override_sha256={copied_dmnt_hash}",
                (
                    f"payload.{index}.previous_dmnt_override_sha256="
                    f"{','.join(previous)}"
                ),
                f"payload.{index}.dmnt_path={target_relative}",
            ]
        )
        generated = dict(build)
        generated["nxsync_dmnt_override_sha256"] = copied_dmnt_hash
        generated["corresponding_source_file"] = source_archive_name
        generated["corresponding_source_sha256"] = source_archive_hash
        generated_builds.append(generated)

    (payload_root / "manifest.ini").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )
    generated_manifest = {
        "schema": "nxsync-installer-staging",
        "version": 2,
        "common_sha256": common_hashes,
        "atmosphere_license_sha256": atmosphere_license_hash,
        "builds": generated_builds,
    }
    (ROOT / "installer" / "payload-manifest.generated.json").write_text(
        json.dumps(generated_manifest, indent=2) + "\n", encoding="utf-8"
    )
    print("Installer payloads staged and verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
