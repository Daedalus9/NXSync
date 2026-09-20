# Vendored overlay components

NXSync vendors the complete source subset used to build its overlay from
`libultrahand` release `v2.5.3`, commit
`856ddbddd796fc4a59ad2e0bf939c5963e6f9dd2`. This directory is not original
NXSync code and must not be covered by the repository-level MIT grant.

The vendored bundle contains these distinguishable components:

| Paths | Component and provenance | License used by NXSync |
| --- | --- | --- |
| `libtesla/include/tesla.hpp`, `libtesla/source/tesla.cpp` and supporting files | ppkantorski's custom fork of [WerWolv/libtesla](https://github.com/WerWolv/libtesla), distributed through [ppkantorski/libultrahand](https://github.com/ppkantorski/libultrahand) | GPL-2.0-or-later; see `libtesla/LICENSE` and the notices in the source files |
| `libultra/` | ppkantorski's libultra, distributed through [ppkantorski/libultrahand](https://github.com/ppkantorski/libultrahand) | GPL-2.0-only AND CC-BY-4.0, following the "licensed under both" notices in its source files; see `libultra/LICENSE` and `libultra/SUB_LICENSE` |
| `common/cJSON.c`, `common/cJSON.h` | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON), version 1.7.18 | MIT; the full notice is retained in both files and at `../../../LICENSES/third-party/cJSON-MIT.txt` |
| `libtesla/include/stb_truetype.h` | [nothings/stb](https://github.com/nothings/stb), stb_truetype 1.26 | MIT alternative selected from the upstream MIT/public-domain dual license; the complete dual-license notice remains in the header and the selected MIT text is at `../../../LICENSES/third-party/stb-MIT.txt` |
| `ultrahand.mk` | libultrahand build integration | Upstream libultrahand terms |

The relative links to `LICENSES/third-party` above are repository paths from
`overlay/lib/libultrahand/`; release archives place equivalent notices below
their top-level `licenses/` directory.

NXSync-specific UI and behavior live in `overlay/source/main.cpp`. That file is
licensed GPL-2.0-only for distribution as part of the linked overlay binary.
Its license does not replace any copyright or license notice in this vendored
directory.
