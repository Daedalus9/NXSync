# Licensing audit — updated 2026-09-18

QR login addition: the main NRO compiles the unchanged MIT-licensed Project Nayuki
QR Code generator 1.8.0, pinned to commit `720f62bddb7226106071d4728c292cb1df519ceb`,
plus the already vendored MIT cJSON 1.7.18. Copyright/license notices are included
in installer assets and the complete source accompanies the release. No Animeita
code or assets are copied. Existing overlay and Atmosphere obligations still apply.

The inspected sources have no identified license incompatibility when distributed
under the component licenses below. NXSync is a mixed-license distribution;
the root MIT license does not cover the entire installer payload. Publication of
the binaries requires **all three corresponding-source assets** and the bundled
notices, not just GitHub's automatic source ZIP.

| Component | Publication terms and evidence |
| --- | --- |
| Original NRO, observer, worker, installer and portable code | MIT, as recorded in `LICENSE` and `REUSE.toml` |
| NXSync overlay and modified Atmosphere dmnt | GPL-2.0-only; exact source and build instructions accompany each release |
| libultrahand v2.5.3, commit `856ddbddd796fc4a59ad2e0bf939c5963e6f9dd2` | Preserve the component-specific GPL, CC-BY-4.0, MIT and attribution notices in `COMPONENTS.md` |
| Mbed TLS 2.28.10 | The archive's `LICENSE` offers Apache-2.0 OR GPL-2.0-or-later. Select the GPL alternative for the overlay. Package metadata saying only `apache` is not the complete upstream license grant |
| libnx 4.12.0, curl 7.69.1, zlib/MiniZip 1.3.1 | ISC, curl and Zlib licenses; exact tarballs, Switch patches and build recipes are checksum-pinned in `dependency-sources.lock.json` |
| SDL2, SDL2_ttf, Mesa/glapi, libdrm, HarfBuzz, FreeType, bzip2, libpng | Used by the separate MIT NRO. Preserve the permissive notices; select FreeType's FTL alternative, including its attribution |
| libgcc/libstdc++ runtime | GPL with GCC Runtime Library Exception 3.1; the exception permits the independent components' existing licenses for eligible compilation output |
| newlib C runtime | Preserve its aggregate per-component notices for the versions in both toolchains; no blanket MIT relicensing |

CC BY 4.0 is compatible with all GNU GPL versions according to the
[FSF license list](https://www.gnu.org/licenses/license-list.html#ccby).
This finding is about CC **BY**, not CC BY-SA. Preserve attribution, the license
link and modification notices as required by the
[CC BY 4.0 terms](https://creativecommons.org/licenses/by/4.0/).

The source bundle includes linked non-system libraries because GPLv2 requires
the modules and scripts needed to rebuild the executable; see
[GPLv2 section 3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3).
GCC's runtime terms are documented by
[the GCC project](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html).
The copied runtime notices have their own origin/hash manifest under
`LICENSES/third-party/runtime-provenance.json`.

## Release enforcement

- Source-tree checks reject tracked generated files, runtime state, Python bytecode
  and recognizable configuration credentials. Git ignores alone are insufficient:
  ignored files already tracked are still checked.
- The installer is checked against the entire staged RomFS, and both dmnt binaries
  must match `compatibility.json` and their corresponding-source manifests.
- The packager verifies every dependency source checksum and generates a dedicated
  NXSync/overlay source asset, two Atmosphere source assets and `SHA256SUMS.txt`.
- The GitHub workflow creates only a **draft prerelease**, from a version tag whose
  commit belongs to `main`. All source assets must stay available with the binary.
- Firmware images used to verify compatibility remain local build evidence.
  The installer distributes only the GPL dmnt overrides and NXSync components.

`check-licensing.py` verifies required files and invariants; it does not determine
copyright ownership or replace review when dependencies change. The original
NXSync authorship is taken from the existing repository notices. The audit covers
copyright-license compatibility and source availability, not trademark clearance,
platform terms or the legality of every possible downstream use.
