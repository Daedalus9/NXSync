#!/usr/bin/env python3
"""Build patched dmnt and inject it into one exact official Atmosphere ROMFS."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def run(arguments: list[str], cwd: Path | None = None) -> None:
    print("+", " ".join(arguments), flush=True)
    subprocess.run(arguments, cwd=cwd, check=True)


def remove_tree(path: Path) -> None:
    """Remove a generated tree, including read-only Git objects on Windows."""
    if not path.exists():
        return

    def make_writable_and_retry(function, name, _error) -> None:
        os.chmod(name, stat.S_IREAD | stat.S_IWRITE)
        function(name)

    shutil.rmtree(path, onerror=make_writable_and_retry)


def git_for(path: Path, *arguments: str) -> list[str]:
    """Use only the explicitly supplied checkout as a Git safe directory."""
    return ["git", "-c", f"safe.directory={path}", *arguments]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def hash_file_tree(root: Path) -> dict[str, str]:
    """Return a stable content map for every regular file below root."""
    return {
        path.relative_to(root).as_posix(): sha256(path)
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def git_tracked_files(checkout: Path) -> list[Path]:
    """Return every tracked source path, including initialized submodules."""
    raw = subprocess.check_output(
        git_for(
            checkout,
            "-C",
            str(checkout),
            "ls-files",
            "-z",
            "--recurse-submodules",
        )
    )
    files: list[Path] = []
    for encoded in raw.split(b"\0"):
        if not encoded:
            continue
        relative = Path(os.fsdecode(encoded))
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(f"Unsafe tracked source path: {relative}")
        source = checkout / relative
        if source.is_dir():
            raise RuntimeError(
                "Corresponding source contains an unexpanded gitlink: "
                f"{relative}. Initialize every submodule before building."
            )
        if not source.exists() and not source.is_symlink():
            raise RuntimeError(f"Tracked source path is unavailable: {relative}")
        files.append(relative)
    return sorted(files, key=lambda item: item.as_posix())


def add_archive_bytes(
    archive: tarfile.TarFile, archive_path: str, data: bytes, mode: int = 0o644
) -> None:
    info = tarfile.TarInfo(archive_path)
    info.size = len(data)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    import io

    archive.addfile(info, io.BytesIO(data))


def add_tracked_tree(
    archive: tarfile.TarFile, checkout: Path, archive_prefix: str
) -> int:
    count = 0
    for relative in git_tracked_files(checkout):
        source = checkout / relative
        archive_path = f"{archive_prefix}/{relative.as_posix()}"
        if source.is_symlink():
            info = tarfile.TarInfo(archive_path)
            info.type = tarfile.SYMTYPE
            info.linkname = os.readlink(source)
            info.mode = 0o777
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            info.mtime = 0
            archive.addfile(info)
        else:
            file_mode = source.stat().st_mode
            mode = 0o755 if file_mode & stat.S_IXUSR else 0o644
            add_archive_bytes(archive, archive_path, source.read_bytes(), mode)
        count += 1
    return count


def create_corresponding_source(
    version: str,
    commit: str,
    toolchain_image: str,
    build_tree: Path,
    patch: Path,
    libnx_checkout: Path | None,
    libnx_commit: str | None,
    output_dir: Path,
) -> tuple[Path, int, int]:
    """Package the exact patched source used by the distributed dmnt binary."""
    changed = subprocess.check_output(
        git_for(
            build_tree,
            "-C",
            str(build_tree),
            "diff",
            "--name-only",
            "HEAD",
            "--",
        ),
        text=True,
    ).splitlines()
    expected_changed = [
        "stratosphere/dmnt/dmnt.json",
        "stratosphere/dmnt/source/cheat/impl/dmnt_cheat_api.cpp"
    ]
    if sorted(changed) != expected_changed:
        raise RuntimeError(
            "Unexpected modified files in Atmosphere corresponding source: "
            + ", ".join(sorted(changed))
        )

    archive_name = (
        f"atmosphere-{version}-nxsync-corresponding-source.tar.xz"
    )
    destination = output_dir / archive_name
    temporary = destination.with_suffix(destination.suffix + ".part")
    if temporary.exists():
        temporary.unlink()
    if destination.exists():
        destination.unlink()

    root_name = f"NXSync-Atmosphere-{version}-corresponding-source"
    source_manifest = {
        "schema": "nxsync-atmosphere-corresponding-source",
        "version": 1,
        "atmosphere_version": version,
        "atmosphere_commit": commit,
        "toolchain_image": toolchain_image,
        "libnx_commit": libnx_commit,
        "nxsync_patch": patch.relative_to(ROOT).as_posix(),
        "nxsync_patch_sha256": sha256(patch),
    }
    instructions = f"""NXSync modified Atmosphere corresponding source
================================================

This archive is the preferred form for modifying the Atmosphere dmnt binary
embedded in the matching NXSync installer release.

Atmosphere version: {version}
Atmosphere commit: {commit}
Toolchain image: {toolchain_image}
libnx override commit: {libnx_commit or 'none (toolchain image version)'}

The `atmosphere/` tree already contains the NXSync patch. The original patch,
compatibility metadata and build helper are under `nxsync-build-inputs/`.
Follow `nxsync-build-inputs/BUILDING.md` and use the recorded version.

This is an unofficial Atmosphere modification. It is not produced, reviewed or
supported by the Atmosphere developers.
"""

    with tarfile.open(
        temporary, mode="w:xz", format=tarfile.GNU_FORMAT
    ) as archive:
        add_archive_bytes(
            archive,
            f"{root_name}/README.txt",
            instructions.encode("utf-8"),
        )
        add_archive_bytes(
            archive,
            f"{root_name}/SOURCE-MANIFEST.json",
            (json.dumps(source_manifest, indent=2) + "\n").encode("utf-8"),
        )
        atmosphere_count = add_tracked_tree(
            archive, build_tree, f"{root_name}/atmosphere"
        )
        libnx_count = 0
        if libnx_checkout is not None:
            libnx_count = add_tracked_tree(
                archive, libnx_checkout, f"{root_name}/libnx"
            )
        inputs = {
            "dmnt-cheat-api.patch": patch,
            "build-atmosphere-payload.py": ROOT
            / "scripts"
            / "build-atmosphere-payload.py",
            "compatibility.json": ROOT / "compatibility.json",
            "BUILDING.md": ROOT / "docs" / "BUILDING.md",
            "NXSync-MIT.txt": ROOT / "LICENSE",
            "NXSync-LICENSE-MAP.md": ROOT / "LICENSES" / "README.md",
        }
        for name, source in inputs.items():
            add_archive_bytes(
                archive,
                f"{root_name}/nxsync-build-inputs/{name}",
                source.read_bytes(),
                0o755 if name.endswith(".py") else 0o644,
            )
    temporary.replace(destination)
    return destination, atmosphere_count, libnx_count


def load_build(version: str) -> dict[str, object]:
    manifest = json.loads((ROOT / "compatibility.json").read_text(encoding="utf-8"))
    if (
        manifest.get("schema") != "nxsync-atmosphere-compatibility"
        or manifest.get("schema_version") != 2
    ):
        raise SystemExit("Unsupported compatibility.json schema")
    for build in manifest["builds"]:
        if build["atmosphere_version"] == version:
            return build
    raise SystemExit(f"Unsupported Atmosphere version: {version}")


def find_dmnt_nsp(worktree: Path) -> Path:
    candidates = sorted(
        worktree.glob("stratosphere/dmnt/out/**/dmnt.nsp"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise RuntimeError("Patched dmnt.nsp was not generated")
    return candidates[0]


def build_dmnt_nsp(target: Path) -> None:
    """Pack the compiled ExeFS in a fixed order, independent of the filesystem."""
    entries = []
    for name, suffix, magic in (
        (b"main", ".nso", b"NSO0"),
        (b"main.npdm", ".npdm", b"META"),
    ):
        source = target.with_suffix(suffix)
        data = source.read_bytes()
        if not data.startswith(magic):
            raise RuntimeError(f"Invalid compiled dmnt input: {source}")
        entries.append((name, data))

    # PFS0 uses little-endian entries and a string table padded to 0x20 bytes.
    names = b"".join(name + b"\0" for name, _ in entries)
    names += b"\0" * (-len(names) % 0x20)
    header = struct.pack("<4sIII", b"PFS0", len(entries), len(names), 0)
    offset = 0
    name_offset = 0
    for name, data in entries:
        header += struct.pack("<QQII", offset, len(data), name_offset, 0)
        offset += len(data)
        name_offset += len(name) + 1
    target.write_bytes(header + names + b"".join(data for _, data in entries))


def download_official_release(build: dict[str, object], version: str) -> Path:
    url = str(build["official_release_url"])
    expected = str(build["official_release_zip_sha256"]).lower()
    cache = ROOT / ".artifacts" / "upstream"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"atmosphere-{version}-official.zip"
    temporary = archive.with_suffix(".zip.part")

    if not archive.is_file():
        if temporary.exists():
            temporary.unlink()
        print(f"Downloading official Atmosphere {version} release...", flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "NXSync-build"})
        with urllib.request.urlopen(request) as response, temporary.open("wb") as out:
            shutil.copyfileobj(response, out)
        temporary.replace(archive)

    actual = sha256(archive)
    if actual != expected:
        raise RuntimeError(
            f"Official Atmosphere archive hash mismatch: {actual} != {expected}"
        )
    return archive


def read_official_files(
    archive: Path, build: dict[str, object]
) -> tuple[bytes, bytes]:
    with zipfile.ZipFile(archive) as release:
        package3 = release.read("atmosphere/package3")
        romfs = release.read("atmosphere/stratosphere.romfs")

    package3_hash = hashlib.sha256(package3).hexdigest()
    expected_package3 = str(build["official_package3_sha256"]).lower()
    if package3_hash != expected_package3:
        raise RuntimeError(
            f"Official package3 hash mismatch: {package3_hash} != {expected_package3}"
        )

    romfs_hash = hashlib.sha256(romfs).hexdigest()
    expected_romfs = str(build["official_stratosphere_romfs_sha256"]).lower()
    if romfs_hash != expected_romfs:
        raise RuntimeError(
            f"Official ROMFS hash mismatch: {romfs_hash} != {expected_romfs}"
        )
    return package3, romfs


def prepare_libnx_checkout(
    build: dict[str, object], source_override: Path | None
) -> tuple[Path | None, str | None]:
    commit_value = build.get("libnx_commit")
    if not commit_value:
        if source_override is not None:
            raise RuntimeError("This Atmosphere build does not use a libnx override")
        return None, None

    commit = str(commit_value)
    checkout = ROOT / ".artifacts" / "dependencies" / f"libnx-{commit[:9]}"
    if checkout.exists():
        actual = subprocess.check_output(
            git_for(checkout, "-C", str(checkout), "rev-parse", "HEAD"),
            text=True,
        ).strip()
        if actual != commit:
            raise RuntimeError(f"Cached libnx HEAD is {actual}; expected {commit}")
        return checkout, commit

    checkout.parent.mkdir(parents=True, exist_ok=True)
    if source_override is not None:
        clone_source = source_override.resolve()
        run(
            git_for(
                clone_source,
                "clone",
                "--local",
                "--no-hardlinks",
                "--no-checkout",
                str(clone_source),
                str(checkout),
            )
        )
    else:
        run(
            [
                "git",
                "clone",
                "--no-local",
                "--no-checkout",
                str(build["libnx_source_url"]),
                str(checkout),
            ]
        )
    run(git_for(checkout, "-C", str(checkout), "checkout", "--detach", commit))
    return checkout, commit


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True, choices=("1.8.0", "1.11.2"))
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / ".artifacts" / "atmosphere",
    )
    parser.add_argument("--keep-build-tree", action="store_true")
    parser.add_argument("--resume-build-tree", action="store_true")
    parser.add_argument("--clean-build-tree", action="store_true")
    parser.add_argument("--libnx-source", type=Path)
    args = parser.parse_args()

    source = args.source.resolve()
    output = args.output.resolve()
    build = load_build(args.version)
    commit = str(build["upstream_commit"])
    branch = str(build["upstream_branch"])
    image = str(build["toolchain_image"])
    patch = (ROOT / str(build["dmnt_patch"])).resolve()
    libnx_checkout, libnx_commit = prepare_libnx_checkout(
        build, args.libnx_source
    )

    try:
        inside_worktree = subprocess.check_output(
            git_for(source, "-C", str(source), "rev-parse", "--is-inside-work-tree"),
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except subprocess.CalledProcessError:
        inside_worktree = "false"
    if inside_worktree != "true":
        raise SystemExit(f"Not an Atmosphere Git checkout: {source}")

    actual_commit = subprocess.check_output(
        git_for(source, "-C", str(source), "rev-parse", "HEAD"), text=True
    ).strip()
    if actual_commit != commit:
        raise SystemExit(
            f"Source HEAD is {actual_commit}; expected exact commit {commit}"
        )

    build_tree = ROOT / ".artifacts" / "build-trees" / f"atmosphere-{args.version}"
    if build_tree.exists() and not args.resume_build_tree:
        raise SystemExit(
            f"Build tree already exists: {build_tree}. Remove it explicitly first."
        )
    if args.resume_build_tree and not build_tree.exists():
        raise SystemExit(f"Build tree does not exist: {build_tree}")
    build_tree.parent.mkdir(parents=True, exist_ok=True)

    created_build_tree = build_tree.exists()
    try:
        if args.resume_build_tree:
            resumed_commit = subprocess.check_output(
                git_for(build_tree, "-C", str(build_tree), "rev-parse", "HEAD"),
                text=True,
            ).strip()
            if resumed_commit != commit:
                raise RuntimeError(
                    f"Build tree HEAD is {resumed_commit}; expected {commit}"
                )
            run(
                git_for(build_tree, "apply", "--reverse", "--check", str(patch)),
                cwd=build_tree,
            )
        else:
            created_build_tree = True
            run(
                [
                    "git",
                    "clone",
                    "--no-local",
                    "--no-checkout",
                    str(source),
                    str(build_tree),
                ]
            )
            run(
                git_for(
                    build_tree,
                    "-C",
                    str(build_tree),
                    "checkout",
                    "--detach",
                    commit,
                )
            )
            run(
                git_for(
                    build_tree, "submodule", "update", "--init", "--recursive"
                ),
                cwd=build_tree,
            )
            run(
                git_for(build_tree, "apply", "--check", str(patch)),
                cwd=build_tree,
            )
            run(git_for(build_tree, "apply", str(patch)), cwd=build_tree)

        mount = f"type=bind,source={build_tree},target=/work"
        docker = ["docker", "run", "--rm"]
        if os.name != "nt" and hasattr(os, "getuid") and hasattr(os, "getgid"):
            docker.extend(["--user", f"{os.getuid()}:{os.getgid()}"])
        docker_base = list(docker)
        docker_base.extend(["--mount", mount, "-w", "/work", image])
        build_docker = docker_base
        libnx_marker = build_tree / ".nxsync-libnx-build"
        if libnx_checkout is not None and libnx_commit is not None:
            isolated_devkit = build_tree / ".nxsync-devkit"
            if isolated_devkit.exists():
                remove_tree(isolated_devkit)
            libnx_mount = f"type=bind,source={libnx_checkout},target=/libnx-src"
            libnx_builder = list(docker)
            libnx_builder.extend(
                [
                    "--mount",
                    mount,
                    "--mount",
                    libnx_mount,
                    "-w",
                    "/work",
                    image,
                    "make",
                    "-C",
                    "/libnx-src",
                    "-j2",
                    "DESTDIR=/work/.nxsync-devkit",
                    "install",
                ]
            )
            run(libnx_builder)
            installed_libnx = (
                isolated_devkit / "opt" / "devkitpro" / "libnx"
            )
            if not (installed_libnx / "include" / "switch.h").is_file():
                raise RuntimeError("Isolated libnx installation is incomplete")
            custom_libnx_mount = (
                f"type=bind,source={installed_libnx},"
                "target=/opt/devkitpro/libnx,readonly"
            )
            build_docker = list(docker)
            build_docker.extend(
                [
                    "--mount",
                    mount,
                    "--mount",
                    custom_libnx_mount,
                    "-w",
                    "/work",
                    image,
                ]
            )

        marker_matches = (
            libnx_marker.is_file()
            and libnx_marker.read_text(encoding="ascii").strip()
            == (libnx_commit or "image")
        )
        if args.clean_build_tree or not marker_matches:
            run(
                build_docker
                + ["make", "-C", "stratosphere/dmnt", "clean-nx_release"]
            )
            run(
                build_docker
                + [
                    "make",
                    "-C",
                    "libraries/libstratosphere",
                    "clean-nx_release",
                ]
            )
        build_dmnt = build_docker + [
            "make",
            "-C",
            "stratosphere/dmnt",
            "-j2",
            "ATMOSPHERE_MAKEFILE_TARGET=nx_release",
            "ATMOSPHERE_BUILD_NAME=release",
            "ATMOSPHERE_BOARD=nx-hac-001",
            "ATMOSPHERE_CPU=arm-cortex-a57",
            f"ATMOSPHERE_GIT_BRANCH={branch}",
            f"ATMOSPHERE_GIT_REVISION={branch}-{commit[:7]}",
            f"ATMOSPHERE_GIT_HASH={commit[:16]}",
        ]
        run(build_dmnt)
        libnx_marker.write_text((libnx_commit or "image") + "\n", encoding="ascii")

        official_archive = download_official_release(build, args.version)
        official_package3, official_romfs = read_official_files(
            official_archive, build
        )

        pack_dir = build_tree / ".nxsync-pack"
        expected_pack_parent = (build_tree / ".nxsync-pack").resolve().parent
        if pack_dir.resolve().parent != expected_pack_parent:
            raise RuntimeError("Unsafe temporary pack path")
        if pack_dir.exists():
            remove_tree(pack_dir)
        extracted = pack_dir / "extracted"
        extracted.mkdir(parents=True)
        official_romfs_path = pack_dir / "official.romfs"
        baseline_romfs_path = pack_dir / "baseline.romfs"
        patched_romfs_path = pack_dir / "stratosphere.romfs"
        official_romfs_path.write_bytes(official_romfs)

        run(
            docker_base
            + [
                "hactool",
                "-t",
                "romfs",
                "--romfsdir=/work/.nxsync-pack/extracted",
                "/work/.nxsync-pack/official.romfs",
            ]
        )
        run(
            docker_base
            + [
                "build_romfs",
                "/work/.nxsync-pack/extracted",
                "/work/.nxsync-pack/baseline.romfs",
            ]
        )
        expected_official_romfs = str(
            build["official_stratosphere_romfs_sha256"]
        ).lower()
        baseline_hash = sha256(baseline_romfs_path)
        if baseline_hash != expected_official_romfs:
            raise RuntimeError(
                "Official ROMFS did not survive an exact extract/repack round-trip: "
                f"{baseline_hash} != {expected_official_romfs}"
            )

        official_tree = hash_file_tree(extracted)
        patched_dmnt = find_dmnt_nsp(build_tree)
        build_dmnt_nsp(patched_dmnt)
        embedded_dmnt = (
            extracted
            / "atmosphere"
            / "contents"
            / "010000000000000d"
            / "exefs.nsp"
        )
        if not embedded_dmnt.is_file():
            raise RuntimeError("Official ROMFS does not contain dmnt")
        shutil.copy2(patched_dmnt, embedded_dmnt)
        patched_dmnt_hash = sha256(embedded_dmnt)
        patched_tree = hash_file_tree(extracted)
        embedded_dmnt_relative = embedded_dmnt.relative_to(extracted).as_posix()
        changed_files = {
            path
            for path in official_tree.keys() | patched_tree.keys()
            if official_tree.get(path) != patched_tree.get(path)
        }
        if changed_files != {embedded_dmnt_relative}:
            raise RuntimeError(
                "Patched ROMFS tree changed files other than dmnt: "
                + ", ".join(sorted(changed_files))
            )
        run(
            docker_base
            + [
                "build_romfs",
                "/work/.nxsync-pack/extracted",
                "/work/.nxsync-pack/stratosphere.romfs",
            ]
        )

        output_dir = output / args.version
        output_dir.mkdir(parents=True, exist_ok=True)
        package3_path = output_dir / "package3"
        romfs_path = output_dir / "stratosphere.romfs"
        dmnt_path = output_dir / "dmnt.nsp"
        license_path = output_dir / "LICENSE"

        package3_path.write_bytes(official_package3)
        shutil.copy2(patched_romfs_path, romfs_path)
        shutil.copy2(patched_dmnt, dmnt_path)
        shutil.copy2(build_tree / "LICENSE", license_path)

        source_archive, atmosphere_source_files, libnx_source_files = (
            create_corresponding_source(
                args.version,
                commit,
                image,
                build_tree,
                patch,
                libnx_checkout,
                libnx_commit,
                output_dir,
            )
        )

        package3_hash = sha256(package3_path)
        romfs_hash = sha256(romfs_path)
        output_dmnt_hash = sha256(dmnt_path)
        if output_dmnt_hash != patched_dmnt_hash:
            raise RuntimeError("Copied dmnt payload hash mismatch")
        metadata = {
            "schema": "nxsync-atmosphere-payload",
            "version": 3,
            "atmosphere_version": args.version,
            "upstream_commit": commit,
            "upstream_branch": branch,
            "official_release_zip_sha256": sha256(official_archive),
            "package3_sha256": package3_hash,
            "official_stratosphere_romfs_sha256": expected_official_romfs,
            "stratosphere_romfs_sha256": romfs_hash,
            "stratosphere_romfs_size": romfs_path.stat().st_size,
            "patched_dmnt_sha256": output_dmnt_hash,
            "official_romfs_roundtrip_verified": True,
            "only_dmnt_changed_verified": True,
            "license_sha256": sha256(license_path),
            "toolchain_image": image,
            "libnx_commit": libnx_commit,
            "corresponding_source_file": source_archive.name,
            "corresponding_source_sha256": sha256(source_archive),
            "corresponding_source_atmosphere_files": atmosphere_source_files,
            "corresponding_source_libnx_files": libnx_source_files,
        }
        (output_dir / "payload.json").write_text(
            json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
        )
        print(json.dumps(metadata, indent=2))
        return 0
    finally:
        if created_build_tree and not args.keep_build_tree and build_tree.exists():
            remove_tree(build_tree)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"Command failed with exit code {error.returncode}", file=sys.stderr)
        raise SystemExit(error.returncode)
