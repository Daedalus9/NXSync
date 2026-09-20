# Third-party notices

The main NRO uses Project Nayuki's QR Code generator 1.8.0 (MIT), vendored at
`third_party/qrcodegen/` from <https://github.com/nayuki/QR-Code-generator>, commit
`720f62bddb7226106071d4728c292cb1df519ceb`. The original files and copyright
headers are unchanged; their SHA-256 hashes are recorded in `UPSTREAM.json`.
The full notice is preserved in `LICENSES/third-party/QR-Code-generator-MIT.txt`
and included in installer releases. The NRO also compiles the MIT cJSON 1.7.18
sources listed below to parse Nextcloud Login Flow v2 responses.
The QR login implementation follows Nextcloud's public protocol; no Animeita
application source or assets are included.

The overlay vendors `libultrahand` release `v2.5.3`, commit
`856ddbddd796fc4a59ad2e0bf939c5963e6f9dd2`, from
<https://github.com/ppkantorski/libultrahand>.

That bundle is not a single body of original NXSync code. It compiles all of
the following into `NXSync.ovl`:

| Vendored component | Provenance | License used by NXSync |
| --- | --- | --- |
| Modified libtesla | Fork by ppkantorski of <https://github.com/WerWolv/libtesla>, shipped by libultrahand | GPL-2.0-or-later |
| libultra | ppkantorski, shipped by libultrahand | GPL-2.0-only AND CC-BY-4.0, following its upstream "licensed under both" notices |
| cJSON 1.7.18 | <https://github.com/DaveGamble/cJSON> | MIT |
| stb_truetype 1.26 | <https://github.com/nothings/stb> | MIT alternative selected from the MIT/public-domain dual license |

The exact file-to-component mapping is preserved in
`overlay/lib/libultrahand/COMPONENTS.md`.

Its upstream metadata and license texts are preserved at:

```text
overlay/lib/libultrahand/UPSTREAM.txt
overlay/lib/libultrahand/LICENSE
overlay/lib/libultrahand/SUB_LICENSE
overlay/lib/libultrahand/libtesla/LICENSE
overlay/lib/libultrahand/libultra/LICENSE
overlay/lib/libultrahand/libultra/SUB_LICENSE
LICENSES/third-party/cJSON-MIT.txt
LICENSES/third-party/stb-MIT.txt
```

NXSync also links or builds against the following projects supplied by the pinned
devkitPro images. They are not vendored here except where explicitly noted:

| Project | License used by NXSync | Upstream |
| --- | --- | --- |
| libnx | ISC | <https://github.com/switchbrew/libnx> |
| SDL2 | Zlib | <https://github.com/libsdl-org/SDL> |
| SDL2_ttf | Zlib | <https://github.com/libsdl-org/SDL_ttf> |
| curl/libcurl | curl license (MIT-style) | <https://github.com/curl/curl> |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later; GPL-2.0-or-later is selected for the GPL overlay | <https://github.com/Mbed-TLS/mbedtls> |
| zlib | Zlib | <https://github.com/madler/zlib> |
| MiniZip | Zlib | <https://github.com/madler/zlib/tree/master/contrib/minizip> |
| Mesa EGL/glapi | MIT-style licenses | <https://gitlab.freedesktop.org/mesa/mesa> |
| libdrm/nouveau | MIT | <https://gitlab.freedesktop.org/mesa/drm> |
| HarfBuzz | MIT | <https://github.com/harfbuzz/harfbuzz> |
| FreeType | FreeType License OR GPL-2.0-only | <https://gitlab.freedesktop.org/freetype/freetype> |
| bzip2 | bzip2 license | <https://sourceware.org/bzip2/> |
| libpng | libpng-2.0 | <https://github.com/pnggroup/libpng> |
| Atmosphère | GPL-2.0-only | <https://github.com/Atmosphere-NX/Atmosphere> |
| GCC libgcc/libstdc++ runtime | GPL-3.0-or-later WITH GCC-exception-3.1 | <https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html> |
| newlib C runtime | Per-component permissive notices in COPYING.NEWLIB | <https://sourceware.org/newlib/> |

The release build currently uses libcurl 7.69.1, SDL2 2.28.5, SDL2_ttf
2.22.0, Mbed TLS 2.28.10, zlib/MiniZip 1.3.1, Mesa EGL/glapi
20.1.0-rc3, HarfBuzz 10.0.1 and libpng 1.6.48 from the pinned devkitPro
image. The exact build environment is defined by the image recorded in the
project workflow. Their licenses do not imply that the upstream projects
endorse NXSync.

Installer release archives contain modified Atmosphère binary material in the
version-specific `dmnt` override payloads. Atmosphère is distributed under the
GNU General Public License version 2. Each installer archive must include the
Atmosphère license, exact upstream commits, NXSync patches, reproducible build
instructions and the complete corresponding source as a sibling release asset.

The installer archive also carries the NXSync license map, the preserved
libultrahand/libtesla/libultra license texts, the dependency license texts
supplied by the pinned devkitPro image and a checksum manifest for those texts.
Replacing a dependency or build image requires repeating the dependency audit.

This software uses the FreeType library (The FreeType Project,
<https://freetype.org/>). The FreeType License alternative is selected for the NRO.
The exact linked overlay dependency sources and devkitPro build recipes accompany
the release in the NXSync corresponding-source asset. See `docs/LICENSING_AUDIT.md`.
