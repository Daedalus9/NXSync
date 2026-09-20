# Licensing and source availability

NXSync is distributed as several separate executables and data payloads. The
license for one component does not replace the license of another component
merely because the installer carries both as separate files.

## NXSync-owned components

The NRO, resident observer, transient worker, installer, shared portable logic,
tests, scripts and documentation are licensed under the MIT License unless a
file or directory says otherwise.

The NXSync-specific overlay source is licensed under GPL-2.0-only because its
resulting binary incorporates GPLv2 libultrahand/Tesla code. The vendored
overlay library is not original NXSync code: it includes a modified libtesla,
libultra, cJSON and stb_truetype under their own upstream terms. The complete
file-to-component map is in `overlay/lib/libultrahand/COMPONENTS.md`. Original
MIT-licensed shared protocol code remains MIT when distributed separately and
is distributed under GPLv2 as part of the linked overlay binary.

The complete corresponding source for `NXSync.ovl` is the dedicated
`NXSync-Installer-<version>-NXSync-corresponding-source.tar.xz` release asset.
It includes the NXSync source tree, the vendored overlay libraries, and the exact
libnx, curl, zlib/MiniZip and Mbed TLS source archives with devkitPro patches and
build recipes. `dependency-sources.lock.json` records their origins and SHA-256
values. A matching GitHub tag alone is insufficient because it omits linked
portlib sources. The packager refuses missing or mismatched dependency sources.

Mbed TLS 2.28.10 is dual licensed. NXSync selects **GPL-2.0-or-later** for its
incorporation into the GPLv2 overlay; the Apache-only choice would be incompatible
with GPLv2. The exact source archive's `LICENSE` retains both alternatives.
For libultra, preserve both GPLv2 and CC-BY-4.0 attribution requirements. CC BY 4.0
is GPL compatible ([FSF license list](https://www.gnu.org/licenses/license-list.html#ccby));
it must not be confused with CC BY-SA 4.0.

## Modified Atmosphere component

The files below describe modifications to Atmosphere and are GPL-2.0-only:

```text
patches/atmosphere/1.11.2/dmnt-cheat-api.patch
patches/atmosphere/1.8.0/dmnt-cheat-api.patch
```

Installer binaries embed the resulting modified `dmnt` executables as separate
payload files. Every binary release must therefore be accompanied by the exact
complete corresponding source generated from the same verified build tree. A
source bundle contains the patched Atmosphere checkout, initialized submodules,
the exact NXSync patch and build inputs, and the pinned libnx checkout when the
build uses one.

The source bundle must be offered from the same release location as the installer
for as long as that binary release remains available. A GitHub-generated source
archive is not a replacement because it may omit submodules and does not contain
the patched Atmosphere tree.

## Third-party software

Upstream copyright and license files are preserved for vendored code. cJSON and
stb notices are also copied into release archives because their source is
compiled into the overlay binary. Release
archives include the Atmosphere GPLv2 text, the NXSync license map,
`THIRD_PARTY_NOTICES.md`, and the dependency license set staged from the pinned
devkitPro image with a SHA-256 manifest. The exact dependency set used for a
release must be reviewed before publication; see `RELEASE_CHECKLIST.md`.

NXSync and its unofficial Atmosphere modification are not produced, reviewed,
approved or supported by Nintendo, the Atmosphere project, Nextcloud or the
Ultrahand project.
