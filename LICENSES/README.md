# License map

NXSync is a multi-component distribution. The repository-level MIT license
does not relicense third-party or derivative components.

| Paths or artifacts | License |
| --- | --- |
| Original NXSync code outside the exceptions below | MIT |
| `third_party/qrcodegen/` | QR Code generator 1.8.0 by Project Nayuki; MIT |
| NXSync-specific `overlay/Makefile`, `overlay/README.md` and `overlay/source/` | GPL-2.0-only |
| `patches/atmosphere/` and modified Atmosphere `dmnt` binaries | GPL-2.0-only |
| `overlay/lib/libultrahand/libtesla/` except `stb_truetype.h` | Modified libtesla; GPL-2.0-or-later, with upstream notices preserved |
| `overlay/lib/libultrahand/libultra/` | GPL-2.0-only AND CC-BY-4.0, following its upstream "licensed under both" notices |
| `overlay/lib/libultrahand/common/cJSON.*` | cJSON 1.7.18; MIT |
| `overlay/lib/libultrahand/libtesla/include/stb_truetype.h` | stb_truetype 1.26; MIT alternative selected from its MIT/public-domain dual license |
| Remaining `overlay/lib/libultrahand/` integration files | Upstream libultrahand terms preserved in that directory |
| Other third-party libraries | Their respective upstream licenses |

The complete GPL version 2 text is retained verbatim at
`overlay/lib/libultrahand/LICENSE`. Installer release archives also include the
exact `LICENSE` copied from the Atmosphere source revision used for the embedded
modified `dmnt` binary.

See `THIRD_PARTY_NOTICES.md` and `docs/LICENSING.md` for attribution and binary
distribution requirements.

The detailed provenance of every source component compiled into the overlay is
recorded in `overlay/lib/libultrahand/COMPONENTS.md`.

Canonical notices for linked permissive dependencies are kept in
`LICENSES/third-party/`. Additional license texts supplied by the pinned
devkitPro image are staged during the build and included under
`licenses/dependencies/` in installer releases.
