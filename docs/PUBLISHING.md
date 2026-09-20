# Publishing the repository and release

The repository is [Daedalus9/NXSync](https://github.com/Daedalus9/NXSync).
Use `dev` for development and `main` for reviewed release commits. Keep the project
files at the repository root. Build outputs and corresponding-source archives stay
outside Git; console configuration, save backups and private diagnostics must not
be included in commits or release assets.

## Local development and first publication

1. Work on `dev`. Before committing, review `git status --short` and the staged diff
   to confirm that only intended source and documentation changes are included.
2. Run the public-tree, version and licensing checks and the host tests described
   in [BUILDING.md](BUILDING.md). No cloud credentials are needed by CI.
3. Push `dev` when development changes are ready to share. Before a release,
   review and merge the intended changes into `main`; if `main` does not exist
   yet, create it from the reviewed development commit. Keep release tags on
   commits reachable from `main`.
4. Enable private vulnerability reporting in the repository settings, check the
   Security tab, and verify CI on GitHub before publishing a release.

Creating a local branch or commit does not publish it. Pushing branches and tags
is a separate step, and release drafts require review before publication.

## Binary releases

Keep the product tag aligned with `versions.json:nro`, currently `v0.30.10-rc2`.
When the intended commit is verified, create that tag on `main` and push the tag.
The installer workflow builds both Atmosphere variants, checks the assets and
creates a **draft prerelease**. A tag does not add another branch.

Review the draft and the checklist before publishing it. The required downloads
are the installer ZIP, **three** corresponding-source archives (NXSync/overlay
and both Atmosphere variants), and `SHA256SUMS.txt`. Publish these together and
keep the sources available with the corresponding binaries. GitHub's automatic
source download does not replace these assets.

The current candidate has host-test and cross-compilation coverage. Console
installation, rollback, interruption and launch tests in `TESTING.md` still need
hardware results; do not describe it as hardware-validated or promote it to stable
until that matrix is complete. Review `LICENSING_AUDIT.md` again when changing
dependencies or toolchains.
