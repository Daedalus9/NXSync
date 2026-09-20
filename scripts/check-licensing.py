#!/usr/bin/env python3
"""Validate NXSync license boundaries and binary-release source obligations."""

from __future__ import annotations

import re
import json
import hashlib
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def require_file(relative: str, violations: list[str]) -> str:
    path = ROOT / relative
    if not path.is_file():
        violations.append(f"missing required license/notice file: {relative}")
        return ""
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        violations.append(f"unreadable license/notice file: {relative}: {error}")
        return ""


def require_text(
    text: str, needle: str, location: str, violations: list[str]
) -> None:
    if needle not in text:
        violations.append(f"{location} does not contain required text: {needle}")


def main() -> int:
    violations: list[str] = []

    qr_provenance = json.loads(require_file("third_party/qrcodegen/UPSTREAM.json", violations))
    for name, expected in qr_provenance["sha256"].items():
        path = ROOT / "third_party/qrcodegen" / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            violations.append("QR generator differs from pinned upstream: " + name)
    qr_notice = require_file("LICENSES/third-party/QR-Code-generator-MIT.txt", violations)
    require_text(qr_notice, "Copyright (c) Project Nayuki", "QR-Code-generator-MIT.txt", violations)
    require_text(qr_notice, "all copies or substantial portions", "QR-Code-generator-MIT.txt", violations)

    provenance = json.loads(require_file("LICENSES/third-party/runtime-provenance.json", violations))
    for entry in provenance:
        path = ROOT / "LICENSES/third-party" / entry["file"]
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["license_sha256"]:
            violations.append("runtime license text differs from upstream: " + entry["file"])
    lock = json.loads(require_file("dependency-sources.lock.json", violations))
    if "@sha256:" not in lock.get("toolchain_image", ""):
        violations.append("dependency-source toolchain is not digest-pinned")
    for entry in lock.get("files", []):
        if not re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", "")):
            violations.append("dependency source lacks a SHA-256: " + entry.get("path", ""))
    require_file("docs/LICENSING_AUDIT.md", violations)
    for marker in ("create_source_release", "NXSync-corresponding-source.tar.xz"):
        require_text(require_file("scripts/package-installer.py", violations), marker,
                     "scripts/package-installer.py", violations)

    root_license = require_file("LICENSE", violations)
    require_text(root_license, "MIT License", "LICENSE", violations)
    require_text(
        root_license,
        "Copyright (c) 2026 Daedalus9",
        "LICENSE",
        violations,
    )

    license_map = require_file("LICENSES/README.md", violations)
    for marker in (
        "Original NXSync code",
        "overlay/",
        "patches/atmosphere/",
        "GPL-2.0-only",
    ):
        require_text(license_map, marker, "LICENSES/README.md", violations)

    for relative in (
        "LICENSES/third-party/libnx-ISC.txt",
        "LICENSES/third-party/SDL2-Zlib.txt",
        "LICENSES/third-party/curl-COPYING.txt",
        "LICENSES/third-party/HarfBuzz-COPYING.txt",
        "LICENSES/third-party/Mesa-MIT.txt",
        "LICENSES/third-party/libdrm-COPYING.txt",
        "LICENSES/third-party/MiniZip-Zlib.txt",
    ):
        require_file(relative, violations)
    license_stager = require_file(
        "scripts/stage-dependency-licenses.sh", violations
    )
    for marker in (
        "switch-mbedtls/LICENSE",
        "switch-freetype/FTL.TXT",
        "switch-libpng/LICENSE",
        "SHA256SUMS.txt",
    ):
        require_text(
            license_stager,
            marker,
            "scripts/stage-dependency-licenses.sh",
            violations,
        )

    gpl_text = require_file("overlay/lib/libultrahand/LICENSE", violations)
    require_text(
        gpl_text,
        "GNU GENERAL PUBLIC LICENSE",
        "overlay/lib/libultrahand/LICENSE",
        violations,
    )
    require_text(
        gpl_text,
        "Version 2, June 1991",
        "overlay/lib/libultrahand/LICENSE",
        violations,
    )
    for relative in (
        "overlay/lib/libultrahand/SUB_LICENSE",
        "overlay/lib/libultrahand/COMPONENTS.md",
        "overlay/lib/libultrahand/libtesla/LICENSE",
        "overlay/lib/libultrahand/libultra/LICENSE",
        "overlay/lib/libultrahand/libultra/SUB_LICENSE",
        "LICENSES/third-party/cJSON-MIT.txt",
        "LICENSES/third-party/stb-MIT.txt",
    ):
        require_file(relative, violations)

    component_map = require_file(
        "overlay/lib/libultrahand/COMPONENTS.md", violations
    ).lower()
    for marker in ("libtesla", "libultra", "cjson", "stb_truetype"):
        require_text(
            component_map,
            marker,
            "overlay/lib/libultrahand/COMPONENTS.md",
            violations,
        )

    reuse = require_file("REUSE.toml", violations)
    for marker in (
        'SPDX-License-Identifier = "MIT"',
        'SPDX-License-Identifier = "GPL-2.0-only"',
        'path = "patches/atmosphere/**"',
    ):
        require_text(reuse, marker, "REUSE.toml", violations)

    licensing = require_file("docs/LICENSING.md", violations)
    for marker in (
        "complete corresponding source",
        "GPL-2.0-only",
        "unofficial Atmosphere modification",
    ):
        require_text(licensing, marker, "docs/LICENSING.md", violations)

    for version in ("1.11.2", "1.8.0"):
        relative = f"patches/atmosphere/{version}/dmnt-cheat-api.patch"
        patch = require_file(relative, violations)
        for marker in (
            "NXSync modification notice:",
            "Modified by Daedalus9 on 2026-09-17.",
            "SPDX-License-Identifier: GPL-2.0-only",
        ):
            require_text(patch, marker, relative, violations)

    notices = require_file("THIRD_PARTY_NOTICES.md", violations).lower()
    for marker in ("libtesla", "libultra", "cjson 1.7.18", "stb_truetype 1.26"):
        require_text(
            notices,
            marker,
            "THIRD_PARTY_NOTICES.md",
            violations,
        )
    dependency_names = {
        "-lnx": "libnx",
        "-lcurl": "libcurl",
        "-lmbedcrypto": "mbed tls",
        "-lmbedtls": "mbed tls",
        "-lmbedx509": "mbed tls",
        "-lz": "zlib",
        "-lminizip": "minizip",
        "-lsdl2": "sdl2",
        "-lsdl2_ttf": "sdl2_ttf",
        "-legl": "mesa egl",
        "-lglapi": "glapi",
        "-ldrm_nouveau": "libdrm",
        "-lharfbuzz": "harfbuzz",
        "-lfreetype": "freetype",
        "-lbz2": "bzip2",
        "-lpng16": "libpng",
    }
    makefiles = (
        "Makefile",
        "cloud-worker/Makefile",
        "overlay/Makefile",
        "sysmodule/Makefile",
        "installer/Makefile",
    )
    linked: set[str] = set()
    for relative in makefiles:
        linked.update(
            re.findall(r"-l[A-Za-z0-9_+.-]+", require_file(relative, violations))
        )
    for token, dependency in dependency_names.items():
        if token in {item.lower() for item in linked} and dependency not in notices:
            violations.append(
                f"THIRD_PARTY_NOTICES.md does not identify linked dependency {token}"
            )

    builder = require_file("scripts/build-atmosphere-payload.py", violations)
    for marker in (
        "create_corresponding_source",
        "git_tracked_files",
        "corresponding_source_sha256",
    ):
        require_text(
            builder, marker, "scripts/build-atmosphere-payload.py", violations
        )

    stager = require_file("scripts/stage-installer-payloads.py", violations)
    require_text(
        stager,
        "Corresponding-source archive mismatch",
        "scripts/stage-installer-payloads.py",
        violations,
    )
    packager = require_file("scripts/package-installer.py", violations)
    for marker in (
        "CORRESPONDING-SOURCE-SHA256SUMS.txt",
        "corresponding-source.tar.xz",
        "THIRD_PARTY_NOTICES.md",
        "licenses/NXSync-MIT.txt",
        "licenses/libultra-CC-BY-4.0.txt",
        "licenses/cJSON-MIT.txt",
        "licenses/stb-MIT.txt",
        "licenses/OVERLAY-COMPONENTS.md",
        "licenses/dependencies/",
        "required_dependency_licenses",
    ):
        require_text(packager, marker, "scripts/package-installer.py", violations)

    readme = require_file("README.md", violations)
    if "A project license has not been selected yet" in readme:
        violations.append("README.md still contains the pre-release license placeholder")
    require_text(readme, "docs/LICENSING.md", "README.md", violations)

    if violations:
        print("Licensing validation failed:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        return 1

    print("Licensing validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
