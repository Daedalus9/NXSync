# Release checklist

## Repository

- [ ] Confirm `LICENSE`, `LICENSES/README.md`, `REUSE.toml` and all third-party
      notices still match the component boundaries and linked dependencies.
- [ ] Use `dev` for development and `main` for reviewed releases; enable private vulnerability reporting.
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

- [ ] Installer `0.1.11-rc2` contains the expected generated payload manifest.
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
- [ ] Offline launch fail-open.
- [ ] Synchronized launch.
- [ ] Cloud-fast-forward launch.
- [ ] Symmetric two-console conflict and multi-parent resolution.
- [ ] emuMMC-only default and passive sysMMC behavior.

- [ ] Run the restore fault matrix in `docs/TESTING.md` before stable promotion.
- [ ] Upload the ZIP, all three corresponding-source assets and `SHA256SUMS.txt`.
- [ ] Verify the draft prerelease contains every file listed by the checksum manifest.
