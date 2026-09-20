#!/usr/bin/env python3
"""Fetch checksum-pinned source archives and the recipes used by the toolchain."""
import hashlib
import json
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def stage() -> None:
    lock = json.loads((ROOT / "dependency-sources.lock.json").read_text())
    target = ROOT / ".artifacts/dependency-sources"
    for entry in lock["files"]:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Unsafe dependency source path")
        path = target / relative
        if not path.is_file():
            path.parent.mkdir(parents=True, exist_ok=True)
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "NXSync-source-release"})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError("Downloaded dependency source mismatch: " + entry["path"])
            temporary = path.with_name(path.name + ".part")
            temporary.write_bytes(data)
            temporary.replace(path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("Cached dependency source mismatch: " + entry["path"])
        print(entry["path"], flush=True)
    print("Dependency sources verified.")


if __name__ == "__main__":
    stage()
