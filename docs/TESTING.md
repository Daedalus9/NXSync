# Testing

## QR account connection

- Connect a test account through Settings > Nextcloud account > Connect with QR.
  Verify scanning in handheld and docked mode, including from a normal TV distance.
- Authorize with a phone, including a 2FA account and an email/SSO login whose
  WebDAV ID differs. Confirm the connection test and upload to the selected folder.
- Cancel before approval; let a session expire; disconnect/reconnect the network.
  The previous saved account must stay usable until a new account is saved.
- Retry an SD write failure after approval. No second phone approval should be
  needed for a local save retry. Revoke unused test grants in Nextcloud afterwards.
- Restart NXSync and verify console-bound encrypted credentials remain readable.

Changes should pass the smallest relevant tests and the full regression suite before
a release candidate is produced.

## Automated tests

Run all host tests:

```sh
sudo apt-get install cmake g++ libmbedtls-dev libcurl4-openssl-dev openssl
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Critical portable coverage includes:

- archive manifest and path validation;
- revision ancestry and conflict classification;
- multi-parent logical conflict resolution;
- preflight and launch protocol serialization;
- atomic publish behavior and truncated-record rejection;
- cloud queue persistence;
- count-based retention;
- emuMMC/sysMMC automation policy;
- lifecycle and worker-launch state transitions;
- encrypted-credential persistence, authentication failure and plaintext migration;
- installer manifest parsing and exact three-input compatibility classification.

## Single-console hardware smoke test

1. Confirm the displayed NRO, observer, worker and overlay versions.
2. Create a manual backup, then immediately repeat a global backup; unchanged saves
   must not create new ZIP archives.
3. Restore a verified local backup up to the profile-selection screen and cancel.
4. Close a modified game and confirm local backup followed by verified cloud upload.
5. Launch with Wi-Fi disabled; preflight must fail open and allow the local save.
6. Confirm that sysMMC remains passive when the default emuMMC-only scope is active.

## Encrypted credential hardware test

1. Start with a working legacy `nextcloud_app_password=` configuration and keep a
   recoverable copy of the application password outside the Switch.
2. Open NXSync once and verify that cloud access still works.
3. Inspect `config.ini`: it must contain
   `nextcloud_app_password_encrypted=v1:...` and no plaintext password field.
4. Reboot Atmosphère and verify both manual cloud test and automatic worker upload.
5. On a second physical Switch, install the same build and confirm that copying only
   the encrypted blob is rejected. Enter the application password through NXSync;
   the second console must then create its own different encrypted blob.

## Two-console conflict test

1. Put both consoles on the same payload and confirm synchronized preflight.
2. Disconnect console B from the network.
3. Modify and upload branch A from console A.
4. Modify branch B independently on offline console B.
5. Reconnect console B and upload its pending backup.
6. Launch the same game on both consoles. Both must report a conflict because neither
   current head is an ancestor of the other.
7. Select one revision, save again and close the game.
8. Inspect the new immutable revision: `parent_revision_ids` must contain both
   conflicting parents.
9. The selected console must then be synchronized. The other console must see a
   cloud fast-forward, not the already-resolved conflict.

## Atmosphère compatibility test

For every supported Atmosphère build:

1. Verify the official `package3` hash.
2. Verify the official `stratosphere.romfs` hash.
3. Build only patched `dmnt` using the pinned toolchain.
4. Extract and repack the official ROMFS unchanged; confirm byte identity.
5. Replace only `dmnt` in the evidence copy, then verify the resulting ROMFS and
   standalone override hashes.
6. Install through the NRO, confirm official `package3` and ROMFS are unchanged,
   cold-boot Atmosphère, and confirm the recorded state.
7. Launch and close several games, including a memory-sensitive title.
8. Exercise synchronized, cloud-newer, conflict, cancel and offline launch paths.
9. Confirm that closing HOME or cancelling the overlay does not leave software stuck
   in `Closing software`.
10. Uninstall through the NRO and verify the override was removed while the exact
    official ROMFS remained unchanged.

The installer must also be tested against an unknown `package3`, an unknown ROMFS,
a corrupted embedded payload, an unknown `010000000000000D` override and recognized
0.1.1 temporary artifacts. Every unsafe case must refuse without a partial install.

Never test an unknown internal Atmosphère module on a console containing the only
copy of a save.

## Release-candidate restore fault matrix

Run on both supported Atmosphere versions, using disposable saves with independent
verified copies. Record the firmware/Atmosphere version, component versions,
profile UID, result and recovery outcome. These cases are not hardware-verified
merely because the host simulations pass.

- Close from HOME during the initial check, before confirming any restore: the
  close popup must finish without waiting for the preflight timeout. Relaunch the
  title and verify a fresh check, including with cheats enabled and disabled.
- Leave the initial check unanswered/offline: continue locally after 30 seconds.
  A late worker result must never authorize save writes or pause the running game.
- Leave the choice open: continue locally after two minutes in that phase.
- Delay a confirmed download beyond five minutes: the game may start locally,
  and the late worker must never write to its save.
- Grant a restore before the download deadline and finish after it: the game
  remains held until a safe result and worker exit.
- Close from HOME while waiting for a choice or downloading, then launch again:
  no previous session's action, phase or grant may affect the new launch.
- Force extraction/commit failure with successful rollback: original files remain
  intact, and only then may the game start.
- Force rollback failure or kill the worker after grant: the game is not started,
  and the recovery guard remains across reboot and automation disablement.
- Cancel via HOME during restore: the game process is terminated. No subsequent
  launch of that title can run until the worker completes safely or recovery is done.
- Recover the matching title/profile through the NRO; verify the marker is removed.
  Recovery to another profile must not remove it.
- Create two profiles with identical/sanitization-colliding names. Back up both and
  exercise retention; neither profile may lose the other's backups. Old nickname
  directories remain readable and are not automatically pruned.
- Install on a fresh SD directory layout, then upgrade from Installer 0.1.9-dev.
  Confirm override identity, cold boot, rollback and uninstall for both variants.

The host suite also executes the actual helper code extracted from both patches,
uses real mbedTLS AES-GCM with only hardware calls stubbed, injects rename failures,
and rejects stale installer RomFS contents.

## Guided automation activation

Run these checks on both supported Atmosphère payloads before a stable release:

- With a fresh disabled configuration and a configured account, cancel the
  preflight activation screen and confirm that `sysmodule.ini` is unchanged.
- Confirm activation: `enabled`, `preflight_enabled` and `backup_on_game_exit`
  become true, while scope and polling interval remain unchanged. With the
  observer stopped, expect **Restart required**, then **Automatic** after reboot.
- Enable preflight with an already running observer: expect **Applying**, then
  **Automatic** without a reboot (within the configured polling interval).
- Disable preflight and close a test game: a backup should still be queued.
  Disable exit backups instead: new exit requests must stop while preflight works.
  Existing queue entries and in-progress operations may complete.
- With emuMMC-only scope on sysMMC, expect **Inactive on sysMMC** and no
  automatic save operations. Retain the user's configured scope during toggles.
- Check missing account, observer, worker, boot flag and overlay dependencies;
  activation must explain what is missing. Keep a stale ready status file with
  the observer stopped: it must never display **Automatic**.
- Update all NXSync components together. An old running observer must show
  **Update required** and must not accept the separate exit backup toggle.
