#!/usr/bin/env python3
"""Ensure user-visible component versions agree with versions.json."""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
VERSIONS = json.loads((ROOT / "versions.json").read_text(encoding="utf-8"))

EXPECTATIONS = {
    "nro": {
        "Makefile": f"APP_VERSION := {VERSIONS['nro']}",
        "source/main.cpp": f'AppVersion = "{VERSIONS["nro"]}"',
        "source/nextcloud_client.cpp": f'NXSync/{VERSIONS["nro"]}',
        "source/nextcloud_login.cpp": f'NXSync/{VERSIONS["nro"]}',
        "compatibility.json": f'"nxsync_release": "{VERSIONS["nro"]}"',
    },
    "sysmodule": {
        "sysmodule/source/main.cpp": f'BuildVersion = "{VERSIONS["sysmodule"]}"',
    },
    "cloud_worker": {
        "cloud-worker/source/main.cpp": f'BuildVersion = "{VERSIONS["cloud_worker"]}"',
    },
    "overlay": {
        "overlay/Makefile": f"APP_VERSION := {VERSIONS['overlay']}",
        "overlay/source/main.cpp": f'OverlayVersion = "{VERSIONS["overlay"]}"',
    },
    "installer": {
        "installer/Makefile": f"APP_VERSION := {VERSIONS['installer']}",
        "installer/source/main.cpp": (
            f'InstallerVersion = "{VERSIONS["installer"]}"'
        ),
    },
}


def main() -> int:
    failures: list[str] = []

    for component, files in EXPECTATIONS.items():
        for relative, expected in files.items():
            path = ROOT / relative
            text = path.read_text(encoding="utf-8")
            if expected not in text:
                failures.append(
                    f"{component}: {relative} does not contain {expected!r}"
                )

    if failures:
        print("Version validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("Version validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
