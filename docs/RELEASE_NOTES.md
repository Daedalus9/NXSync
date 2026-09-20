# NXSync 0.30.11-rc1

This candidate adds guided activation in **Settings > Game launch preflight**
and an independent **Automatic backups after game exit** setting. First activation
asks before enabling background automation, preserves the NAND scope and explains
when a full restart is needed. Settings checks the running observer and refreshes
its reported state. Existing enabled configurations keep exit backups enabled
unless the new setting is explicitly disabled. Install all components together:
the new status format requires the updated NRO and overlay.

**Settings > Nextcloud account > Connect with QR** remains available. Enter the
server and backup folder, scan with your phone, sign in and grant access. NXSync
uses Nextcloud Login Flow v2 and stores the resulting application password using
the existing console-bound encryption. Manual setup remains available.

QR login supports separate sign-in names and WebDAV user IDs, expires after
20 minutes and can be cancelled without replacing the saved account. See
[NEXTCLOUD_LOGIN.md](NEXTCLOUD_LOGIN.md) for requirements and limitations.

This release candidate changes the launch protocol. Install the NRO, observer,
worker, overlay and matching dmnt override together using Installer 0.1.12-rc1,
then cold-boot Atmosphere. Mixing this worker with older launch patches disables
automatic restores because the worker will not receive a valid permission grant.

- Restores require explicit permission from the gate holding the game process.
  A request arriving after the preflight timeout cannot modify a running game's save.
- Downloads must match the archive hash in the filename as well as any server
  checksum. Automatic restores also require the archive revision and payload hash
  to match the selected cloud revision before requesting permission to modify saves.
- Once restore permission is granted, the gate waits for completion. An unsafe
  rollback, worker crash or post-grant timeout blocks the launch. A durable marker
  protects the affected title across restarts; see [recovery](RECOVERY.md).
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
[TESTING.md](TESTING.md), including cancellation during restore, power interruption,
first installation and upgrades from the previous installer on both supported
Atmosphere versions. No Nintendo firmware, games, saves or credentials are included.
