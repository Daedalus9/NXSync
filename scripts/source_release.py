"""Create the complete NXSync/overlay source asset without generated/private files."""
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path


def public_files(root: Path) -> list[Path]:
    if (root / ".git").exists():
        raw = subprocess.check_output(["git", "-c", f"safe.directory={root.as_posix()}",
            "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"])
        names = sorted(set(name.decode() for name in raw.split(b"\0") if name))
    else:
        # Isolated builds carry the source inventory made before compilation.
        names = json.loads((root / "source-inventory.json").read_text())
    for name in names:
        if Path(name).is_absolute() or ".." in Path(name).parts or not (root / name).is_file():
            raise ValueError("Unsafe or missing source: " + name)
    return [root / name for name in names]


def create_source_release(root: Path, destination: Path) -> tuple[Path, str]:
    lock = json.loads((root / "dependency-sources.lock.json").read_text())
    files = {"NXSync/" + path.relative_to(root).as_posix(): path for path in public_files(root)}
    for entry in lock["files"]:
        path = root / ".artifacts/dependency-sources" / entry["path"]
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise RuntimeError("Missing/mismatched corresponding dependency source: " + entry["path"])
        files["dependencies/" + entry["path"]] = path
    if destination.exists():
        raise RuntimeError("Source release already exists: " + str(destination))
    checksums = []
    temporary = destination.with_suffix(destination.suffix + ".part")
    with tarfile.open(temporary, "w:xz") as archive:
        def add(name: str, data: bytes) -> None:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755 if name.endswith(".sh") else 0o644
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
        for name, path in sorted(files.items()):
            data = path.read_bytes()
            checksums.append(hashlib.sha256(data).hexdigest() + "  " + name + "\n")
            add(name, data)
        add("SHA256SUMS.txt", "".join(checksums).encode())
        add("README-SOURCE.txt", (
            "NXSync and overlay corresponding source\n\n"
            "NXSync/ contains the complete project and vendored overlay libraries.\n"
            "dependencies/upstream/ contains the exact libnx, curl, zlib (including\n"
            "MiniZip) and Mbed TLS source tarballs; recipes/ contains the matching\n"
            "devkitPro build recipes, patches and toolchain variable scripts.\n"
            "See NXSync/docs/BUILDING.md and dependency-sources.lock.json for\n"
            "the pinned build image, package versions, origins and hashes.\n"
            "The GPL option is selected for Mbed TLS in the GPL overlay.\n"
            "Atmosphere source is in the two additional sibling source assets.\n"
        ).encode())
    temporary.replace(destination)
    return destination, hashlib.sha256(destination.read_bytes()).hexdigest()
