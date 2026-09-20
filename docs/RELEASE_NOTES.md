# NXSync 0.30.12-rc2

This candidate updates game launch preflight to allow closing the game from HOME
during the initial cloud check. Startup is held through a debug attachment while
the process is started, instead of leaving it in a state that rejects termination.
The initial check now has a 30-second limit. Choosing a save has a separate
two-minute limit, and a confirmed cloud download has a five-minute limit. Before
restore permission is granted, expiry continues with the local save; late requests
cannot obtain permission to modify the running game's save.

NXSync and its installer now have distinct Homebrew Menu icons: the cloud with an
upload arrow for NXSync, and the download arrow for the installer. Original PNGs
and the JPEGs embedded in the NROs are included in the source tree.

Install all components together using **NXSync Installer 0.1.13-rc2**, then
cold-boot Atmosphère. The installer selects the override for an exact supported
Atmosphère 1.8.0 or 1.11.2 build. Updating the complete package keeps the observer,
overlay and launch gate aligned on settings and phase deadlines.

| Component | Included version |
| --- | --- |
| NXSync NRO | `0.30.12-rc2` |
| Resident observer | `0.14.6-rc1` |
| Transient worker | `0.8.9-rc1` |
| Ultrahand overlay | `0.3.22-rc1` |
| Installer | `0.1.13-rc2` |

Enable launch checks through **Settings > Game launch preflight**. The separate
**Automatic backups after game exit** setting controls exit backups. First
activation asks before enabling background automation, preserves the NAND scope and explains
when a full restart is needed. Settings checks the running observer and refreshes
its reported state. Existing enabled configurations keep exit backups enabled
unless that setting is explicitly disabled.

**Settings > Nextcloud account > Connect with QR** remains available. Enter the
server and backup folder, scan with your phone, sign in and grant access. NXSync
uses Nextcloud Login Flow v2 and stores the resulting application password using
the existing console-bound encryption. Manual setup remains available.

QR login supports separate sign-in names and WebDAV user IDs, expires after
20 minutes and can be cancelled without replacing the saved account. See
[NEXTCLOUD_LOGIN.md](https://github.com/Daedalus9/NXSync/blob/v0.30.12-rc2/docs/NEXTCLOUD_LOGIN.md) for requirements and limitations.

- Restores require user confirmation and explicit permission from the gate holding
  the game process.
  A request arriving after the preflight timeout cannot modify a running game's save.
- Downloads must match the archive hash in the filename as well as any server
  checksum. Automatic restores also require the archive revision and payload hash
  to match the selected cloud revision before requesting permission to modify saves.
- Once restore permission is granted, the shorter deadlines no longer release the
  game. Closing from HOME terminates the game while retaining restore protection.
  An unsafe rollback, worker crash or 30-minute post-grant timeout blocks the
  launch. A durable marker protects the affected title across restarts; see
  [recovery](https://github.com/Daedalus9/NXSync/blob/v0.30.12-rc2/docs/RECOVERY.md).
  Backups, uploads and retention for that title pause until recovery succeeds.
- Backups use the profile UID in folder names. Existing nickname-based folders
  remain readable and are excluded from automatic retention deletion.
- Durable protocol/state files retain their previous generation during replacement.
  Queued uploads are replaced only after the successor has been persisted.
- The installer supports a clean SD directory layout and always refreshes its RomFS.
  Packaging checks every embedded payload against staging.
- Release assets include the NXSync/overlay source and exact linked dependency
  sources, both patched Atmosphere source trees, license texts and checksums.

Host fault-injection tests and cross-compilation do not replace console testing.
Before promotion to a stable release, complete the hardware matrix in
[TESTING.md](https://github.com/Daedalus9/NXSync/blob/v0.30.12-rc2/docs/TESTING.md), including HOME followed by closing the game during the
initial check, the separate phase timeouts, cancellation during restore, power
interruption, first installation and upgrades on both supported Atmosphère
versions. No Nintendo firmware, games, saves or credentials are included.
