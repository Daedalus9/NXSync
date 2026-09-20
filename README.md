# NXSync

NXSync is an experimental Nintendo Switch homebrew project that creates verified
save backups, stores them locally, and synchronizes them with a Nextcloud server.
With [background automation](#background-automation) enabled, it automatically
backs up and uploads changed saves after a game closes. With game launch preflight
enabled, it automatically checks cloud saves before a game starts and lets you
restore a newer cloud save after confirmation or choose which revision to use
when the histories diverge.

> [!WARNING]
> NXSync is development software for consoles running Atmosphère. The launch gate
> requires a version-specific Atmosphère build with the NXSync `dmnt` changes
> installed as an exact content override. Use the installer: it hashes `package3`,
> the official `stratosphere.romfs` and any existing `dmnt` override, and refuses
> unknown combinations. A mismatched internal module can prevent software from
> launching or crash Atmosphère.
> The embedded `dmnt` is an unofficial Atmosphère modification. It is not produced,
> reviewed, approved or supported by the Atmosphère developers.

NXSync is not affiliated with Nintendo, Atmosphère, Nextcloud, Ultrahand, or
devkitPro.

## Features

- verified v2 ZIP backups with canonical payload SHA-256 hashes;
- local backup, restore, safety backup, and post-restore verification;
- Nextcloud connection through a phone QR code, with manual WebDAV setup available;
- per-device and per-profile remote folders;
- immutable revision records with one or more parent revisions;
- fast-forward, same-content, cloud-newer, local-newer, unrelated, and conflict
  classification;
- explicit conflict resolution without pretending to merge game-specific files;
- automatic local backup and cloud upload after a game closes;
- launch preflight with an Ultrahand overlay choice before a game opens;
- count-based retention, with five backups per console, profile and game by default;
- optional emuMMC-scoped automation and local launch on pre-restore network failure;
- host-side tests for the portable protocol and revision logic.

## Getting started

1. Check [Supported Atmosphère revisions](#supported-atmosphère-revisions). Before
   installing or updating, copy `atmosphere/` and `config/` from the SD card to a PC
   or external drive, plus `switch/NXSync/` and `switch/.overlays/` if present. See
   [Before installing](docs/INSTALLATION.md#before-installing) for backup details.
   Keep an independent copy of important saves as well.
2. From an installer release ZIP, copy the `switch/NXSyncInstaller/` directory to
   `switch/` on the SD card. Open NXSync Installer from the Homebrew Menu, review
   its compatibility result, press **A**, then confirm with **ZL + ZR + A**.
3. Cold-boot Atmosphère, then open NXSync from the Homebrew Menu.
4. Follow [Configuration](#configuration) to connect Nextcloud. Select a save and
   press **A** to create a first manual backup; check the reported result before
   enabling [background automation](#background-automation).

See [Installation and removal](docs/INSTALLATION.md) for overlay dependencies,
upgrades and uninstalling before an Atmosphère update. To build your own installer,
follow [Building](docs/BUILDING.md).

## Components

Component versions are recorded in [`versions.json`](versions.json).

| Component | Current development build | Purpose |
| --- | --- | --- |
| `NXSync.nro` | `0.30.12-rc2` | GUI, configuration, catalog, manual backup/restore and diagnostics |
| Resident observer | `0.14.6-rc1` | Watches application lifecycle events and queues work |
| Transient worker | `0.8.9-rc1` | Executes preflight, backup, restore and Nextcloud operations |
| Ultrahand overlay | `0.3.22-rc1` | Shows status and asks the user to resolve launch conflicts |
| Installer NRO | `0.1.13-rc2` | Selects and installs only an exact compatible Atmosphère payload |
| Atmosphère integration | version-specific | Patched `dmnt` content override selected by exact Atmosphère hashes |

The resident observer intentionally does not create ZIP archives or contact
Nextcloud. Expensive work runs in the transient worker so memory is returned when
the operation finishes.

## Supported Atmosphère revisions

The current source tree contains patches for these exact upstream revisions:

| Atmosphère | Upstream commit | devkitA64 image |
| --- | --- | --- |
| `1.11.2` | `5388824be146a89619e8d641acd64599cf1c5f62` | `devkitpro/devkita64:20260219` |
| `1.8.0` | `c6014b533fb3b53998bc2cbd2608769fd44a5bc1` | `devkitpro/devkita64:20241023` |

Compatibility is exact, not a version range. See
[`docs/ATMOSPHERE_COMPATIBILITY.md`](docs/ATMOSPHERE_COMPATIBILITY.md).

## Repository layout

```text
assets/icons/         Original artwork and embedded homebrew-menu icons
include/nxsync/       Shared public headers
source/               NRO implementation and shared portable logic
sysmodule/            Resident lifecycle observer (program ID 4200000000004E58)
cloud-worker/         Transient worker (program ID 4200000000004E59)
overlay/              Ultrahand/Tesla overlay
installer/            Offline compatibility-detecting installer NRO
third_party/          Vendored QR generator and upstream provenance
patches/atmosphere/   Version-specific dmnt source patches
tests/                Host-side unit and protocol tests
docs/                 Architecture, formats, compatibility and testing notes
scripts/              Reproducible local/Docker helper scripts
```

Generated binaries, release archives, console configuration, save data, cloud
indexes and crash reports are deliberately excluded from this repository.

## Runtime requirements

- one of the exact builds in [Supported Atmosphère revisions](#supported-atmosphère-revisions);
- a working Internet connection, a Nextcloud account reachable over HTTPS with a
  trusted certificate, and the correct console date/time for cloud transfers;
- `nx-ovlloader`, its `boot2.flag`, and a Tesla-compatible `ovlmenu.ovl` for
  automatic launch preflight and interactive conflict resolution.

The installer reports the overlay dependency files separately. Missing overlay
support does not block manual backup, restore, or installation, but the homebrew
and resident observer refuse to enable automatic launch preflight until all three
files are detected.

## Build

Requirements:

- devkitPro/devkitA64 with the Switch development packages;
- libnx;
- SDL2 and SDL2_ttf Switch portlibs;
- libcurl, mbedTLS, zlib and MiniZip Switch portlibs;
- CMake and C and C++17 compilers for host tests; see [Host tests](#host-tests) for dependencies.

From a devkitPro shell:

```sh
make components
```

Or use the pinned Docker helper:

```powershell
./scripts/build-with-docker.ps1
```

Atmosphère payloads are built separately from their exact upstream commits, then
staged into the installer before the installer NRO is compiled. Detailed commands
are in [`docs/BUILDING.md`](docs/BUILDING.md).

Every installer release includes three corresponding-source archives: one for
NXSync and its linked dependencies, and one for each supported Atmosphère revision.
The Atmosphère archives contain the patched tree and initialized build dependencies;
a GitHub-generated source ZIP is not a substitute. Publish the installer, all three
source archives and `SHA256SUMS.txt` together.

The installer archive also contains the dependency license set staged from the
pinned devkitPro image and a SHA-256 manifest for those texts.

Manual installation paths and the safe Atmosphère update/removal procedure are in
[`docs/INSTALLATION.md`](docs/INSTALLATION.md).

## Host tests

Install CMake, C and C++17 compilers, Python 3, OpenSSL and the Mbed TLS/libcurl development
libraries (`libmbedtls-dev libcurl4-openssl-dev openssl` on Ubuntu). For pinned sources, use
`bash scripts/test-host.sh` in the documented Docker environment.

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

## Configuration

1. Open **Settings > Nextcloud account > Connect with QR**.
2. Enter your Nextcloud server's HTTPS address, for example
   `https://cloud.example.com/nextcloud`.
3. Leave **Remote folder** set to **`NXSync`** unless you want a different backup
   location in your Nextcloud account. This is the folder where cloud backups are
   stored. NXSync creates it during the first upload and organizes backups inside
   it by console, profile and game.
4. Scan the QR code with your phone, sign in to Nextcloud and grant NXSync access.
   Keep NXSync open until it saves the connection and displays the connection test
   result.

For a new setup, the default is `/NXSync/` in your Nextcloud files. Configure the
same Nextcloud account and remote folder on your consoles to share that backup
location. Using the same folder name in separate accounts does not share their files.
**Manual setup** remains available in the account menu. See
[QR connection](docs/NEXTCLOUD_LOGIN.md) for cancellation, expiry and troubleshooting.

NXSync stores runtime configuration at:

```text
sdmc:/config/NXSync/config.ini
```

Use [`config.example.ini`](config.example.ini) as documentation. NXSync stores the
Nextcloud application password as an AES-GCM authenticated credential bound to the
physical console through SPL. A legacy plaintext credential is migrated on first
load. The encrypted blob cannot be used on another console; configure the account
again after moving the SD card to different hardware. Runtime configuration must
still never be committed, attached to an issue, or included in a release archive.

Local backups are stored under `sdmc:/switch/NXSync/backups/` and remain available
without an Internet connection. With the default remote folder, cloud archives
use this layout:

```text
/NXSync/<device-id>/profile-<profile-uid>/<title-id>/<timestamp>_<archive-hash>.zip
```

Global head and immutable revision metadata live below `/NXSync/_index/`. See
[`docs/CLOUD_FORMAT.md`](docs/CLOUD_FORMAT.md) for the revision graph and retention
rules.

### Background automation

On a fresh installation, the resident observer and game launch preflight are
**disabled**. Connecting Nextcloud alone does not enable them. Existing
`sysmodule.ini` settings are preserved during an upgrade.

After configuring Nextcloud, open **Settings > Game launch preflight** and press
**A**. NXSync checks the installed dependencies. If background automation is off,
confirm **Enable** to activate preflight and automatic backups/uploads after games
close. **B** cancels without changing the configuration. If **Restart required**
appears, fully restart Atmosphère and check Settings again. **Automatic** is shown
only after the running sysmodule reports that the feature is active; Settings
refreshes this status while open.

The initial cloud check waits up to **30 seconds**, then continues with the local
save if no decision is available. Choosing a cloud save has separate time limits:
two minutes for the choice and five minutes for its download. Closing the game
from HOME cancels the pending launch. If a confirmed restore is already writing
save data, recovery protection remains until it finishes safely.

**Automatic backups after game exit** can also be enabled on its own. The two
settings are independent once background automation is enabled: turning off
preflight preserves exit backups, and turning off exit backups preserves preflight.
Already queued or running work may finish. These controls preserve the configured
`automation_scope`; the default `emummc` scope shows **Inactive on sysMMC**.
**Dependencies missing** and **Module error** include the reported cause.

For manual configuration, `enabled` is the master switch, `preflight_enabled`
controls launch checks and `backup_on_game_exit` controls new exit backup requests
in `sdmc:/config/NXSync/sysmodule.ini`. Older configurations without
`backup_on_game_exit` retain the previous behavior (exit backups when `enabled=true`).

**Automatic backup at startup** is a separate setting: it runs when you open
NXSync and retries pending uploads. It does not enable the resident observer.

## Safety model

- Restores require explicit confirmation and create a local safety backup when a
  destination save already exists.
- Archive paths, manifest identity, CRC values, file counts, sizes and hashes are
  validated before a save is modified.
- Network failures before restore permission are fail-open. Once a restore is
  granted, unsafe failure blocks launch and preserves a recovery marker; see
  [recovery instructions](docs/RECOVERY.md).
- Automation defaults to emuMMC only.
- NXSync does not merge the internal files of two independently modified saves.
  Conflict resolution selects one payload and records both parent revisions in the
  next logical revision.

I am not liable for damage arising from its use, including console bricks, loss or corruption of save data, or console bans.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) before submitting changes. Changes to
restore, launch-gate, revision, or retention behavior must include tests and a
hardware validation note.

Maintainers: follow [the publication procedure](docs/PUBLISHING.md) and
[release checklist](RELEASE_CHECKLIST.md) when creating the public repository or a release.

## License

Original NXSync code is licensed under the MIT License except for explicitly
identified GPL components:

| Component | License |
| --- | --- |
| Original NRO, observer, worker, installer, shared logic, scripts and tests | MIT |
| NXSync overlay source | GPL-2.0-only |
| Atmosphère patches and modified `dmnt` binaries | GPL-2.0-only |
| Vendored third-party code | Its preserved upstream license |

See [`LICENSE`](LICENSE), [`LICENSES/README.md`](LICENSES/README.md),
[`docs/LICENSING.md`](docs/LICENSING.md) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Distributing an installer
binary without its matching complete corresponding-source assets is not a supported
NXSync release.

NXSync was developed with assistance from a large language model (LLM).
