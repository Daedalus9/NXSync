# Release checklist

Target: NXSync `0.30.12-rc2`, tag `v0.30.12-rc2`, Installer `0.1.13-rc2`.
Check the complete component version set against [`versions.json`](versions.json).

## Repository

- [ ] Confirm `LICENSE`, `LICENSES/README.md`, `REUSE.toml` and all third-party
      notices still match the component boundaries and linked dependencies.
- [ ] Set `main` as the default branch and confirm the release commit is on `main`.
- [ ] Confirm the release tag is `v` followed by the NRO version in `versions.json`.
- [ ] Enable private vulnerability reporting.
- [ ] Confirm all documentation versions match the source.
- [ ] Confirm `git status` contains no generated binaries or local runtime files.
- [ ] Run the public-tree scanner.
- [ ] Run the licensing/source-obligation scanner.
- [ ] Confirm the release tag contains the complete vendored overlay tree,
      including libtesla, libultra, cJSON, stb_truetype and their notices.
- [ ] Run all host tests.
- [ ] Build all common Switch components with the pinned image.

## Atmosphère variants

- [ ] Build each supported Atmosphère payload from its exact upstream commit.
- [ ] Verify the official release ZIP, `package3` and `stratosphere.romfs` hashes.
- [ ] Confirm an unchanged official ROMFS extract/repack is byte-identical.
- [ ] Confirm only the embedded `dmnt` differs in the patched build-evidence ROMFS.
- [ ] Record and verify each generated ROMFS-evidence and standalone `dmnt` hash.
- [ ] Generate and verify one complete corresponding-source archive for every
      distributed `dmnt`, including initialized submodules and pinned libnx source.
- [ ] Perform a cold-boot and launch/exit hardware test for each variant.
- [ ] Stage every verified variant into one installer NRO.
- [ ] Test rejection of unknown identities and unknown `dmnt` overrides.
- [ ] Test verified override uninstall with official Atmosphère files unchanged.
- [ ] Publish the installer rather than user-selectable raw Atmosphère payloads.

## Package contents

- [ ] Installer `0.1.13-rc2` contains the expected generated payload manifest.
- [ ] The embedded NXSync NRO is `0.30.12-rc2`; the observer, worker and overlay
      versions match `versions.json` and the README component table.
- [ ] Homebrew Menu shows the download icon for the installer and the cloud/upload
      icon for NXSync; the corresponding original PNGs are in the source archive.
- [ ] QR login passes real-console scanning, authorization, cancellation and encrypted restart checks.
- [ ] NRO, observer, worker and overlay versions are mutually compatible.
- [ ] No `config.ini`, password, device ID, queue, status, backup, `_index`, log or
      crash file is present.
- [ ] Every packaged file has a SHA-256 manifest.
- [ ] ZIP integrity and internal paths have been verified.
- [ ] Installation and uninstall instructions are included.
- [ ] Atmosphère license and corresponding-source obligations are satisfied.
- [ ] The dedicated NXSync/overlay corresponding-source asset includes exact
      linked dependency tarballs, patches and recipes from `dependency-sources.lock.json`.
- [ ] Every source-asset filename and SHA-256 in the installer ZIP matches the
      sibling release download.
- [ ] NXSync, Atmosphère, libultrahand/Tesla and dependency notices are included.

## Hardware regression

- [ ] Manual backup and restore validation.
- [ ] Unchanged-save deduplication.
- [ ] Automatic backup and upload after game exit.
- [ ] Initial cloud check continues locally after 30 seconds without a decision.
- [ ] Close from HOME during the initial check, before confirming a restore:
      the close popup finishes without waiting for the preflight timeout.
- [ ] Relaunch after cancelling a check, choice or download; the previous session
      cannot affect the new launch, with cheats both enabled and disabled.
- [ ] Choice and confirmed-download limits are two and five minutes respectively;
      late requests cannot obtain permission to modify a running game's save.
- [ ] A restore already granted remains protected after HOME cancellation and
      cannot release the game on a pre-restore timeout.
- [ ] Synchronized launch.
- [ ] Cloud-fast-forward launch.
- [ ] Symmetric two-console conflict and multi-parent resolution.
- [ ] emuMMC-only default and passive sysMMC behavior.

- [ ] Run the restore fault matrix in `docs/TESTING.md` before stable promotion.
- [ ] Upload the ZIP, all three corresponding-source assets and `SHA256SUMS.txt`.
- [ ] Verify the draft prerelease contains every file listed by the checksum manifest.
- [ ] Confirm the draft release notes and all five uploaded assets come from the
      tagged commit before publishing the prerelease.
