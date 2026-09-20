# Changelog

## 0.30.12-rc2 - 2026-09-20

- Add distinct homebrew-menu icons for NXSync and NXSync Installer using the supplied artwork.
- Track icon files as build dependencies and retain their original PNG sources.
- Package the new icons with Installer 0.1.13-rc2; retain the preflight fixes from rc1.

## 0.30.12-rc1 - 2026-09-20

- Hold game startup through a debug attachment so HOME can terminate a preflight session.
- Detect closing through process state and preserve the recovery guard after an interrupted restore.
- Limit the initial cloud check to 30 seconds, with separate choice, download and restore deadlines.
- Add regression simulations for closing, late grants, phase deadlines and debug-handle ownership.
- Package both supported Atmosphère patches and updated components with Installer 0.1.13-rc1.

## 0.30.11-rc1 - 2026-09-20

- Add guided preflight activation, including explicit opt-in to background backups.
- Add an independent game-exit backup setting and preserve existing INI behavior.
- Report restart, NAND scope, dependency and module errors in live Settings.
- Match status to the running observer process before confirming automation is active.
- Package the updated NRO, observer and overlay with Installer 0.1.12-rc1.

## 0.30.10-rc2 - 2026-09-18

- Add Nextcloud Login Flow v2 with an on-screen QR code and cancellable polling.
- Resolve the WebDAV user ID independently of the sign-in name through OCS.
- Preserve the saved account on cancellation or failed authorization; retain manual setup.
- Validate HTTPS, response limits and endpoint origins before exchanging credentials.
- Include the MIT QR generator license, pinned provenance and source in release assets.
- Add HTTPS integration coverage and package the new GUI with Installer 0.1.11-rc2.

## 0.30.9-rc1 - 2026-09-17

- Added restore permission handshake, explicit abort results and persistent launch recovery guard.
- Bound cloud downloads to filename hashes and automatic restores to the selected revision and payload.
- Isolated backup retention by profile UID; preserved legacy nickname directories.
- Reworked durable file replacement and queue publication; added failure-injection coverage.
- Fixed clean-SD installer directory creation and stale embedded payload detection.
- Added real AES-GCM and dmnt gate simulation tests; assertions remain active in Release.
- Pinned CI actions/toolchain digests and added draft prerelease publishing from main.
- Completed dependency corresponding-source packaging and runtime license notices.

## Previous development snapshot

Repository preparation snapshot:

- NRO `0.30.8-dev`;
- resident observer `0.14.3-dev`;
- transient worker `0.8.7-dev`;
- Ultrahand overlay `0.3.19-dev`;
- installer `0.1.9-dev` with per-component licensing, complete Atmosphère
  corresponding-source assets and release-time license validation;
- Nextcloud application passwords are stored as AES-GCM authenticated blobs
  using an SPL-derived device-unique key; legacy plaintext configuration is
  migrated automatically and copied/tampered credentials fail closed;
- conflict and cloud-update screens now render save metadata as non-focusable
  information rows and reserve selectable styling for explicit actions only;
- restore actions no longer duplicate the A-button hint in their labels, and
  restore progress and power warnings are rendered as non-focusable text;
- cloud-restore progress and safety messages use separate left-aligned rows so
  no right-hand value can overlap a long status label;
- all user-facing homebrew, overlay, notification, worker, installer,
  configuration, and diagnostic text converted to English;
- count-based cloud retention and revision-DAG conflict detection;
- logical multi-parent conflict resolution;
- automatic close backup and cloud upload;
- automatic launch preflight through version-specific Atmosphère `dmnt` patches;
- atomic launch-request publication with truncated-record rejection;
- patches validated for Atmosphère `1.8.0` and `1.11.2` exact upstream commits;
- offline installer with exact package3, official-ROMFS and override
  detection;
- installer, homebrew and resident observer detect the nx-ovlloader binary,
  boot flag and overlay menu; automatic launch preflight remains blocked when
  any required overlay dependency is missing, while manual backup remains
  available;
- version-specific `dmnt` content overrides, leaving active Atmosphère files
  unchanged, with atomic install, verified removal and conservative upgrades;
- safe cleanup of hash-recognized partial files left by installer `0.1.1-dev`;
- reproducible builds that inject only the matching patched `dmnt` into an
  SHA-256-verified official `stratosphere.romfs`;
- portable installer-manifest and compatibility-classification tests;
- installer packaged in its own `switch/NXSyncInstaller/` hbmenu application
  directory so it remains visible beside the main NXSync NRO.

Earlier experimental build archives are intentionally not part of the source
repository history.
