# Installation and removal

Use `NXSyncInstaller.nro` for installation. The installer contains all supported
version-specific payloads and chooses one only after exact SHA-256 matching; the
user does not select an Atmosphère version manually.

The installer contains an unofficial modified Atmosphère `dmnt` binary. Every
official NXSync release provides the matching complete corresponding-source archive
beside the installer download. NXSync is not produced, reviewed or supported by the
Atmosphère developers.

## Before installing

Before installing or updating NXSync, copy these existing SD folders to a dated
backup on a PC or external drive:

- `atmosphere/`: the current Atmosphère files, configuration and all content
  overrides and sysmodules under `atmosphere/contents/`.
- `config/`: NXSync settings and configuration for other homebrew tools.
- `switch/NXSync/` and `switch/.overlays/`, if present: the previous NXSync app,
  its local backups and the installed overlays.

Fully power off the console before removing the SD card. Check that the copy
completed successfully and record the installed Atmosphère version with it; do
not mix files from different Atmosphère versions during recovery. Keep an
independent copy of important game saves as well.

The installer adds or updates NXSync modules and the `dmnt` override under
`atmosphere/contents/`. It leaves the recognized official `package3` and
`stratosphere.romfs` unchanged and preserves an existing
`config/NXSync/sysmodule.ini`. Its temporary rollback files are not a persistent
backup of your Atmosphère setup or configuration.

## Installation steps

1. Complete the backups above.
2. Copy the installer to `sdmc:/switch/NXSyncInstaller/NXSyncInstaller.nro`.
3. Launch it from the Homebrew Menu.
4. Review the detected Atmosphère version and state.
5. Press `A`, then confirm with `ZL + ZR + A`.
6. Cold-boot Atmosphère after a successful installation.

The installer also checks for the runtime overlay dependency files:

- `atmosphere/contents/420000000007E51A/exefs.nsp`;
- `atmosphere/contents/420000000007E51A/flags/boot2.flag`;
- `switch/.overlays/ovlmenu.ovl`.

These files are not bundled or modified by NXSync. If any are missing, the
installer shows a warning and automatic launch preflight remains blocked. Manual
backup, restore, and cloud operations remain available. Install a compatible
Tesla/Ultrahand environment separately, reboot Atmosphère, then enable preflight
from NXSync.

The installer hashes:

```text
sdmc:/atmosphere/package3
sdmc:/atmosphere/stratosphere.romfs
sdmc:/atmosphere/contents/010000000000000D/exefs.nsp (if present)
```

It performs no installation when either file is absent, unknown or an unrecognized
modification. It also verifies every embedded payload before writing.

## Installed SD paths

```text
switch/NXSync/NXSync.nro
switch/.overlays/NXSync.ovl
atmosphere/contents/4200000000004E58/exefs.nsp
atmosphere/contents/4200000000004E58/flags/boot2.flag
atmosphere/contents/4200000000004E59/exefs.nsp
atmosphere/contents/010000000000000D/exefs.nsp
config/NXSync/sysmodule.ini
config/NXSync/installer.lock
```

The version-specific `dmnt` is installed as a content override. The active
`package3` and `stratosphere.romfs` remain byte-identical to the recognized official
Atmosphère release.

The installer does not overwrite an existing `sysmodule.ini`. Nextcloud credentials,
queues and backups are not embedded in the installer.

NXSync encrypts the Nextcloud application password with AES-GCM and a device-unique
key derived by the Switch Security Processor. Existing plaintext configuration is
migrated automatically on first load. Because the credential is bound to the
physical console, copying `config.ini` to another Switch requires entering the
application password again.

## Updating Atmosphère

Run the installer and choose uninstall (`X`, then `ZL + ZR + X`) before changing
Atmosphère. This removes the recognized override and NXSync executable components
whose hashes still match the embedded files.

An official Atmosphère update normally replaces both `package3` and
`stratosphere.romfs`, but explicit uninstall remains the safest procedure. After the
update, run the newest installer. If the new two-file identity is not allowlisted,
installation is intentionally blocked until an exact payload is published.

## Enable background automation

After connecting Nextcloud, open **Settings > Game launch preflight** and confirm
**Enable**. On first activation this also enables backups and uploads after game
exit. Restart Atmosphère if prompted, then verify **Automatic** in Settings.
To enable only exit backups, use **Automatic backups after game exit** instead.
Disabling either setting preserves the other; queued work may still finish.
The default scope remains emuMMC only. See [background automation](../README.md#background-automation).

## Removing NXSync

Use the installer's uninstall action. It preserves:

- `config/NXSync/config.ini` and other runtime/configuration data;
- local save backups below `switch/NXSync/backups/`;
- files that no longer match the installer payload, to avoid deleting user or
  third-party modifications.

Delete configuration and backups separately only when they are no longer needed
and another recovery copy exists.

## Existing `dmnt` override

Older development packages used:

```text
atmosphere/contents/010000000000000D/exefs.nsp
```

This is the current installation path. The installer upgrades or removes it only
if its hash is one of the exact recorded NXSync hashes. If an unknown override
exists, installation stops and reports the conflict; it never deletes the file
blindly.

If installer 0.1.1 ever succeeded in replacing the active ROMFS, restore the exact
official Atmosphère `package3` and `stratosphere.romfs` from a PC first. An NRO
cannot safely replace the ROMFS while Atmosphère keeps it open.
