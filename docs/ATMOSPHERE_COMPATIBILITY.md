# Atmosphère compatibility

NXSync's automatic launch preflight patches Atmosphère's internal `dmnt` system
module. `dmnt` is not stored in `package3`: Atmosphère packages it inside
`atmosphere/stratosphere.romfs`. Internal modules do not have a cross-release
binary compatibility guarantee, so every supported Atmosphère revision has a
separate source patch, toolchain and `dmnt` override payload.

## Supported builds

| Atmosphère | Exact upstream commit | Official `package3` SHA-256 | Official `stratosphere.romfs` SHA-256 | Toolchain image | Extra dependency |
| --- | --- | --- | --- | --- | --- |
| `1.11.2` | `5388824be146a89619e8d641acd64599cf1c5f62` | `F162A419887374028103E097DC5679F97B3B22501FEE667405A3BC965EEAA3F2` | `A49C9D4846DD204E92678DB7D0D494F4FC4F6F78943CD245AF38C15CC187130B` | `devkitpro/devkita64:20260219` | image `libnx` |
| `1.8.0` | `c6014b533fb3b53998bc2cbd2608769fd44a5bc1` | `B991622C2EF17B2631CAFABDFE6BEBB8EB39C523CFF5D862890F047784A1DC83` | `EE41EAD8B90AA8C5FF9BDCF7825A8B78D967EB8603E1497B72D766CFC8E28C78` | `devkitpro/devkita64:20241023` | `libnx` `250a5777f7833a60da2434cab567bcf9263a1967` |

The official hashes form a two-file identity. Matching only the visible version
string, or only `package3`, is insufficient because a user may already have a
modified or mismatched `stratosphere.romfs`.

Machine-readable values, current and previous override hashes, and the integrated
ROMFS build-evidence hashes are stored in
[`compatibility.json`](../compatibility.json).

## Installer decisions

The installer hashes both identity files and the override path before it offers an
action:

- recognized `package3`, official ROMFS and no override: install;
- recognized `package3`, official ROMFS and current override: already installed;
- recognized `package3`, official ROMFS and an allowlisted older override: upgrade;
- unknown override or any other ROMFS: refuse without changing the SD card.

The installer writes only the content override and NXSync components. Writes use a
staged file, exact post-copy hash verification and a rollback rename. Uninstall
removes only files whose hashes are recognized; it does not alter Atmosphère's
active `package3` or `stratosphere.romfs`.

An older `atmosphere/contents/010000000000000D/exefs.nsp` is upgraded or removed
only when its SHA-256 exactly matches a recorded NXSync build. An unknown file at
that path blocks installation rather than deleting another project's data.

Installer 0.1.2 also removes exact, hash-recognized temporary files left by the
failed 0.1.1 integrated-ROMFS strategy. If the active ROMFS itself is an integrated
NXSync build, the installer blocks: restore the official Atmosphère files from a PC
and rerun it.

## Rules

1. Never install a payload produced for a different Atmosphère revision.
2. Do not infer compatibility from a newer or older version number.
3. Build only from the recorded commit and pinned Docker image.
4. Verify the downloaded official release, `package3` and ROMFS against pinned hashes.
5. Verify an unchanged ROMFS extract/repack is byte-identical, then replace only
   `dmnt` in a build-evidence copy and record both the resulting ROMFS and standalone
   `dmnt` hashes before release.
6. Release only the installer NRO; do not publish ambiguous per-version folders
   for users to select manually.

## Patch locations

```text
patches/atmosphere/1.11.2/dmnt-cheat-api.patch
patches/atmosphere/1.8.0/dmnt-cheat-api.patch
```

The repository contains patches and reproducible build metadata, not generated
Atmosphère binaries. Release artifacts may embed the patched `dmnt` overrides,
subject to Atmosphère's license and corresponding-source obligations.
