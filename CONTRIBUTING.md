# Contributing

NXSync modifies and restores save data, so changes should be small, reviewable and
tested proportionally to their risk.

## Development rules

- Do not commit console configuration, Nextcloud credentials, device identifiers,
  profile UIDs, save archives, cloud indexes, crash dumps or screenshots containing
  personal data.
- Use synthetic identifiers in tests and documentation.
- Do not commit build outputs or upstream Atmosphère checkouts.
- Keep the resident observer lightweight. Save mounting, compression, restore and
  network operations belong in the transient worker or NRO.
- Keep launch failures fail-open unless the user has explicitly selected a verified
  cloud restore.
- Never infer conflict resolution from timestamps alone.
- Preserve atomic publish and strict parser validation for every cross-process file.

## Before submitting a change

1. Build all affected Switch components.
2. Run the complete host test suite.
3. Add regression coverage for portable logic.
4. Document hardware validation for lifecycle, launch-gate, restore or Atmosphère
   changes.
5. Update every visible component version when the release protocol requires it.
6. Scan the proposed tree for secrets and generated files.

Changes to the Atmosphère patch must be maintained separately for every supported
upstream commit and must record the exact toolchain and output hash.

## Contribution licensing

By submitting a contribution, you certify that you have the right to provide it
under the license assigned to its path in `LICENSES/README.md` and `REUSE.toml`.
Do not copy Nintendo proprietary code, firmware, keys, game content or confidential
SDK material into the project. Atmosphère modifications must retain upstream
copyright notices, carry a dated NXSync modification notice and remain
GPL-2.0-only.
