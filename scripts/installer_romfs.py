"""Read a Switch RomFS without extracting paths or trusting its metadata."""
from __future__ import annotations

import hashlib
import struct
from pathlib import Path

NONE = 0xFFFFFFFF


def romfs_hashes(data: bytes) -> dict[str, str]:
    if len(data) < 80:
        raise ValueError("Truncated RomFS header")
    header, dh, dhs, dirs, ds, fh, fhs, files, fs, payload = struct.unpack_from("<10Q", data)
    if header != 80 or payload < header or payload > len(data):
        raise ValueError("Invalid RomFS header")
    metadata = []
    for start, size in ((dh, dhs), (dirs, ds), (fh, fhs), (files, fs)):
        # elf2nro stores file data before the metadata tables. Both layouts
        # are valid; payload is a base offset, not the end of metadata.
        if start < header or start + size > len(data):
            raise ValueError("RomFS metadata outside its range")
        if size:
            if any(start < end and start + size > begin for begin, end in metadata):
                raise ValueError("Overlapping RomFS metadata")
            metadata.append((start, start + size))
    result: dict[str, str] = {}
    seen_dirs: set[int] = set()
    seen_files: set[int] = set()
    seen_paths: set[str] = set()

    def name_at(table: int, size: int, offset: int, length: int) -> str:
        if offset + length > size:
            raise ValueError("Truncated RomFS name")
        name = data[table + offset:table + offset + length].decode("utf-8")
        if name in (".", "..") or any(c in name for c in "/\\\x00"):
            raise ValueError("Unsafe RomFS name")
        return name

    pending = [(0, "", 0)]
    while pending:
        offset, parent_path, expected_parent = pending.pop()
        if offset in seen_dirs or offset + 24 > ds:
            raise ValueError("Invalid/cyclic RomFS directory")
        seen_dirs.add(offset)
        parent, sibling, child, file, _, length = struct.unpack_from("<6I", data, dirs + offset)
        if parent != expected_parent:
            raise ValueError("Invalid RomFS directory parent")
        name = name_at(dirs, ds, offset + 24, length)
        if (offset == 0 and name) or (offset != 0 and not name):
            raise ValueError("Invalid RomFS directory name")
        path = f"{parent_path}/{name}" if parent_path else name
        if path in seen_paths:
            raise ValueError("Duplicate RomFS path")
        seen_paths.add(path)
        if sibling != NONE:
            if offset == 0:
                raise ValueError("RomFS root has a sibling")
            pending.append((sibling, parent_path, expected_parent))
        if child != NONE:
            pending.append((child, path, offset))
        while file != NONE:
            if file in seen_files or file + 32 > fs:
                raise ValueError("Invalid/cyclic RomFS file")
            seen_files.add(file)
            owner, sibling, start, size, _, length = struct.unpack_from("<IIQQII", data, files + file)
            if owner != offset or payload + start + size > len(data):
                raise ValueError("Invalid RomFS file range/parent")
            if size and any(payload + start < end and payload + start + size > begin
                            for begin, end in metadata):
                raise ValueError("RomFS file overlaps metadata")
            name = name_at(files, fs, file + 32, length)
            full_path = f"{path}/{name}" if path else name
            if not name or full_path in seen_paths:
                raise ValueError("Invalid/duplicate RomFS file name")
            seen_paths.add(full_path)
            result[full_path] = hashlib.sha256(data[payload + start:payload + start + size]).hexdigest()
            file = sibling
    return result


def verify_staged_romfs(data: bytes, staging: Path) -> None:
    actual = romfs_hashes(data)
    expected = {
        path.relative_to(staging).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in staging.rglob("*") if path.is_file()
    }
    if not expected:
        raise ValueError("Installer staging is empty")
    if actual != expected:
        changed = sorted(k for k in actual.keys() | expected.keys() if actual.get(k) != expected.get(k))
        raise ValueError("Installer embeds stale or unexpected payloads: " + ", ".join(changed))
