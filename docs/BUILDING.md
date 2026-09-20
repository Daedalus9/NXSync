# Building

## Common NXSync components

The NRO, resident observer, transient worker and overlay use:

```text
devkitpro/devkita64:20260219
```

From a devkitPro shell:

```sh
make components
```

Outputs:

```text
NXSync.nro
sysmodule/nxsync-sysmodule.nsp
cloud-worker/nxsync-cloud-worker.nsp
overlay/NXSync.ovl
```

The Docker helpers build these common components and run host tests:

```powershell
./scripts/build-with-docker.ps1
```

```sh
./scripts/build-with-docker.sh
```

The homebrew-menu icons are committed under `assets/icons/`: `nxsync.jpg` for
NXSync and `installer.jpg` for the installer. Both are 256 x 256 RGB JPEGs; the
original PNG artwork is retained alongside them. No image converter is needed
to build the project, and changes to an icon trigger regeneration of its NRO.

## Host tests only

```sh
sudo apt-get install cmake g++ libmbedtls-dev libcurl4-openssl-dev openssl
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The Docker helpers build host Mbed TLS and libcurl from the checksum-pinned
release sources with `scripts/test-host.sh`. The QR login test uses an isolated
HTTPS server and a temporary test certificate, without a real Nextcloud account.

## Version-specific Atmosphère payloads

Clone Atmosphère once with submodules. Check out one of the exact commits from
[`compatibility.json`](../compatibility.json); the helper refuses any other HEAD.

```sh
git clone --recursive https://github.com/Atmosphere-NX/Atmosphere.git atmosphere-src
git -C atmosphere-src checkout 5388824be146a89619e8d641acd64599cf1c5f62
git -C atmosphere-src submodule update --init --recursive
python3 scripts/build-atmosphere-payload.py \
  --version 1.11.2 --source atmosphere-src
```

Repeat with the `1.8.0` commit and `--version 1.8.0`. The script creates a temporary
standalone clone, apply-checks the appropriate patch, and builds only `dmnt` with
the pinned toolchain. It downloads the exact official release archive recorded in
`compatibility.json`, verifies the archive, `package3` and ROMFS hashes, extracts
the official ROMFS, and verifies that an unchanged extract/repack is byte-identical.
It then replaces only the embedded `dmnt` and produces:

```text
.artifacts/atmosphere/<version>/package3
.artifacts/atmosphere/<version>/stratosphere.romfs
.artifacts/atmosphere/<version>/dmnt.nsp
.artifacts/atmosphere/<version>/payload.json
.artifacts/atmosphere/<version>/atmosphere-<version>-nxsync-corresponding-source.tar.xz
```

Atmosphère 1.8.0 additionally pins the exact `libnx` commit recorded in
`compatibility.json`. The helper builds it into an isolated staging directory and
bind-mounts it over the container's `libnx` only for that `dmnt` build; it does not
modify the Docker image or host devkitPro installation.

The copied `package3` remains the byte-identical official file and is never installed
or modified by NXSync; it is retained as build evidence and installer identity data.
The patched ROMFS remains build evidence proving that only `dmnt` changed. Its hash
is checked against `nxsync_stratosphere_romfs_sha256`; the standalone override is
checked against `nxsync_dmnt_override_sha256`. The installer stages only `dmnt.nsp`
and never installs `package3` or the generated ROMFS.

Before the temporary build tree is removed, the helper archives every tracked file
from the patched Atmosphère checkout and initialized submodules. When a pinned
libnx override is used, its complete tracked source is archived as well. The source
archive also contains the exact patch, compatibility manifest, build helper,
instructions and license map. Its SHA-256 is recorded in `payload.json` and verified
again during staging and packaging.

## Installer NRO

Build the common components and both Atmosphère payloads first. Then stage only
verified files into the installer's embedded RomFS:

```sh
python3 scripts/stage-installer-payloads.py
make installer
```

Output:

```text
installer/NXSyncInstaller.nro
```

Create the user-facing archive only after all ROMFS-evidence and standalone `dmnt`
hashes are recorded:

```sh
python3 scripts/stage-dependency-sources.py
python3 scripts/package-installer.py
```

Packaging produces the installer ZIP, a complete NXSync/overlay source asset with
the linked dependencies, and one corresponding-source `tar.xz` asset for every
supported Atmosphère build. The installer ZIP contains the
NXSync and third-party license notices, source-asset filenames and SHA-256 values.
Raw version-specific executable payloads are not exposed for manual selection.

Staged binaries and generated manifests are ignored by Git. Run the public-tree
scanner against the Git source tree to ensure no generated payload, secret or runtime file was
added accidentally.

Do not commit generated `.nsp`, `.nso`, `.nro`, `.ovl`, `.elf`, ROMFS, ZIP, source
archive, build, Atmosphère checkout or `.artifacts` files. Corresponding-source
archives belong to the matching binary release, not to Git history.

## Rebuilding linked overlay dependencies

`dependency-sources.lock.json` identifies the exact source archives and devkitPro
recipe revision for libnx, curl, Mbed TLS, zlib/MiniZip and toolchain variable
scripts. `stage-dependency-sources.py` verifies their SHA-256 values. The dedicated
NXSync source release includes these under `dependencies/` along with the project.
To rebuild a library, unpack its upstream tarball, apply the patches beside its
`PKGBUILD`, and follow that recipe's `build()`/`package()` steps using the recorded
Docker image and `/opt/devkitpro/switchvars.sh`. Preserve the GPL option for Mbed
TLS when linking the overlay. Compiler/runtime components use their standard
system-library/runtime-exception terms; their notices are also shipped.

Docker image digests are pinned in workflows, helpers and `compatibility.json`.
The GitHub installer workflow can be started manually for a selected branch.
It creates a **draft prerelease** only for a `v<versions.json:nro>` tag reachable
from `main`. Develop on `dev` and review changes before promoting them to `main`.
Preserve all three source assets with the ZIP.
