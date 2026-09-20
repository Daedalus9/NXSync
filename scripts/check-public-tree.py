#!/usr/bin/env python3
"""Reject generated, runtime, cloud and credential files from the public tree."""

from __future__ import annotations

import re
import sys
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent

BLOCKED_DIRECTORY_PREFIXES = (
    "build",
    "release-",
    "verify-",
    ".tmp-",
    ".inspect-",
)

BLOCKED_DIRECTORY_NAMES = {
    ".artifacts",
    "__pycache__",
    "dist",
    "backups",
    "queue",
    "_index",
    "fatal_errors",
    "fatal_reports",
    "crash_reports",
}

BLOCKED_FILE_NAMES = {
    "config.ini",
    "device_id.txt",
    "compatibility.lock",
    "cloud-worker.status",
    "sysmodule.status",
    "preflight.status",
    "launch.request",
    "launch.decision",
    "launch.action",
    "launch.restore-claim",
    "launch.restore-grant",
    "launch.restore-guard",
}

BLOCKED_SUFFIXES = {
    ".7z",
    ".pyc",
    ".pyo",
    ".bak",
    ".bin",
    ".elf",
    ".log",
    ".nacp",
    ".npdm",
    ".nro",
    ".nso",
    ".nsp",
    ".ovl",
    ".romfs",
    ".tar",
    ".xz",
    ".zip",
}

SECRET_PATTERNS = (
    re.compile(r"(?im)^\s*nextcloud_username\s*=\s*[^#\s].+$"),
    re.compile(r"(?im)^\s*nextcloud_app_password\s*=\s*[^#\s].+$"),
    re.compile(r"(?im)^\s*nextcloud_app_password_encrypted\s*=\s*[^#\s].+$"),
    re.compile(r"(?i)authorization\s*:\s*(?:basic|bearer)\s+[a-z0-9+/=._-]+"),
)

TEXT_SUFFIXES = {
    "",
    ".c",
    ".cpp",
    ".h",
    ".hpp",
    ".ini",
    ".json",
    ".md",
    ".mk",
    ".patch",
    ".ps1",
    ".py",
    ".sh",
    ".txt",
    ".yml",
    ".yaml",
}


def is_blocked_directory(path: Path) -> bool:
    return any(
        part in BLOCKED_DIRECTORY_NAMES
        or any(part.startswith(prefix) for prefix in BLOCKED_DIRECTORY_PREFIXES)
        for part in path.parts
    )


def main() -> int:
    violations: list[str] = []

    if (ROOT / ".git").exists():
        raw = subprocess.check_output(["git", "-c", f"safe.directory={ROOT.as_posix()}",
            "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard", "-z"])
        paths = [ROOT / name.decode("utf-8") for name in sorted(set(raw.split(b"\0"))) if name]
    else:
        paths = sorted(p for p in ROOT.rglob("*") if p.is_file())
    for path in paths:
        relative = path.relative_to(ROOT)
        if is_blocked_directory(relative.parent):
            violations.append(f"blocked directory content: {relative}")
            continue
        if not path.is_file():
            violations.append(f"missing public file: {relative}")
            continue
        if path.name in BLOCKED_FILE_NAMES:
            violations.append(f"runtime file: {relative}")
        if path.suffix.lower() in BLOCKED_SUFFIXES:
            violations.append(f"generated/binary file: {relative}")

        if path.suffix.lower() not in TEXT_SUFFIXES:
            continue

        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            violations.append(f"unreadable or non-UTF-8 public text: {relative}")
            continue

        for pattern in SECRET_PATTERNS:
            if pattern.search(text):
                violations.append(f"possible credential: {relative}")
                break

    if violations:
        print("Public-tree validation failed:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        return 1

    print("Public-tree validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
