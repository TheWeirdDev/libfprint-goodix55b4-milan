# Releasing

How to cut a release of `libfprint-goodix55b4`. Written for AI agents, but
it is the same procedure for people. Follow the steps in order: the AUR
package builds from the GitHub tag, so GitHub must be done first.

## Where things live

| What | Where |
|---|---|
| Driver source, PKGBUILD, changelog | this repo, `git@github.com:TheWeirdDev/libfprint-goodix55b4-milan.git`, branch `master` |
| Release workflow | `.github/workflows/release.yml` (runs on every `v*` tag push) |
| AUR package | `ssh://aur@aur.archlinux.org/libfprint-goodix55b4.git`, conventionally cloned to `~/aur/libfprint-goodix55b4` (holds only `PKGBUILD` and `.SRCINFO`) |

## Versioning

`pkgver` is `<libfprint version>.m<n>`, e.g. `1.94.10.m5`. The git tag is
`v` + `pkgver` (`v1.94.10.m5`); the workflow fails if they differ.

- Driver or packaging change: bump `m<n>`, keep `pkgrel=1`.
- PKGBUILD-only fix for an already-released version (no source change):
  bump `pkgrel` instead; this needs no new tag, only steps 6-7.
- New upstream libfprint: change the version prefix and `_libfprint_commit`
  in the PKGBUILD and `REV` in `scripts/build-local.sh`, reset to `m1`.

## Before you start

- Ask the maintainer before pushing, tagging, merging PRs or publishing to
  the AUR. Each is public and hard to undo; agreeing to one is not agreeing
  to the others.
- `git status` must be clean apart from known untracked local files (e.g.
  `finger.pgm`, old `*.pkg.tar` in `packaging/arch/`). Never `git add -A`;
  add files by name.
- Be on `master`, up to date: `git pull --rebase origin master`.
- Build locally once: `scripts/build-local.sh` (builds into `.build/`,
  installs nothing). It must finish without errors or new warnings.

## Steps

1. **Changelog.** In `CHANGELOG.md`, add a section at the top, below the
   intro:

   ```
   ## v1.94.10.m6 - YYYY-MM-DD

   - What changed, written for users of the package.
   ```

   The heading must be exactly `## <tag> - <date>`: the workflow uses the
   second word of the heading to find the section, and uses the lines up to
   the next `## ` as the GitHub release notes. If there is a
   `## Unreleased` section, rename it instead. Credit outside contributors
   with their handle and PR number.

2. **Bump `pkgver`** in `packaging/arch/PKGBUILD` and make sure `pkgrel=1`.

3. **Commit** the changelog and the PKGBUILD together:

   ```
   git add CHANGELOG.md packaging/arch/PKGBUILD
   git commit -m "PKGBUILD: 1.94.10.m6"
   ```

4. **Tag** (annotated, like every earlier tag) and push the branch and the
   tag together:

   ```
   git tag -a v1.94.10.m6 -m v1.94.10.m6
   git push origin master v1.94.10.m6
   ```

5. **Wait for the release workflow and check it.**

   ```
   gh run list -R TheWeirdDev/libfprint-goodix55b4-milan -L 1
   gh run watch <run-id> -R TheWeirdDev/libfprint-goodix55b4-milan --exit-status
   gh release view v1.94.10.m6 -R TheWeirdDev/libfprint-goodix55b4-milan
   ```

   The release must have the changelog section as its notes and these
   eight files:

   - `libfprint-goodix55b4-<pkgver>-1-x86_64.pkg.tar.zst` (Arch)
   - `libfprint-goodix55b4_<pkgver>-1_<release>_amd64.deb` for `debian12`,
     `debian13`, `ubuntu24.04` and `ubuntu26.04`
   - `libfprint-goodix55b4-<pkgver>-1.fc<NN>.x86_64.rpm` for `fc43` and `fc44`
   - `SHA256SUMS`

   The release job runs only when every build succeeded. If the run failed, read `gh run view <run-id> --log-failed`,
   fix it on `master`, and rebuild the same tag with
   `gh workflow run release.yml -R TheWeirdDev/libfprint-goodix55b4-milan -f tag=v1.94.10.m6`
   (a rebuild replaces the release's notes and files). Do not move or
   re-push a tag that has been pushed.

   The workflow builds with the same PKGBUILD the AUR uses, fetching the
   driver at the tag from GitHub, so a green run means the AUR package
   builds too.

   To test packaging changes before tagging, push the branch and start a
   build-only run (empty tag; nothing is published):
   `gh workflow run release.yml -R TheWeirdDev/libfprint-goodix55b4-milan --ref <branch>`.
   Locally, `packaging/debian/build-deb.sh` and `packaging/fedora/build-rpm.sh`
   build the same packages inside a container of the target release.

6. **Update the AUR package.**

   ```
   cd ~/aur/libfprint-goodix55b4      # or: git clone ssh://aur@aur.archlinux.org/libfprint-goodix55b4.git
   git pull
   cp <repo>/packaging/arch/PKGBUILD .
   makepkg --printsrcinfo > .SRCINFO
   git diff                           # expect only pkgver/pkgrel (and source tag) changes, unless you meant more
   git add PKGBUILD .SRCINFO
   git commit -m "1.94.10.m6-1: <one-line summary>"
   git push origin master
   ```

   Always regenerate `.SRCINFO`; the AUR rejects a push where it does not
   match the PKGBUILD. Commit only `PKGBUILD` and `.SRCINFO`, never build
   output.

7. **Report** to the maintainer: the release URL, the AUR commit, and the
   install commands from the release notes (`yay -S libfprint-goodix55b4`
   on Arch; `apt install ./…deb` or `dnf install --allowerasing ./…rpm` with
   the file from the release elsewhere).

## Notes

- The workflow uses the workflow's own commit to read `CHANGELOG.md`: the
  tag on a tag push, `master` on a manual rebuild. Older tags can therefore
  be rebuilt with notes as long as `master`'s changelog has their section.
- Tags before v1.94.100.m2 have no `packaging/debian` or
  `packaging/fedora`, so a manual rebuild of them fails in the deb and rpm
  jobs and publishes nothing; their releases keep the Arch package only.
- `-debug` packages are turned off in CI (`OPTIONS+=(!debug)` in the build
  user's `~/.makepkg.conf`).
- The workflow does not publish to the AUR; that would need the AUR SSH key
  as a repository secret, which has not been set up.
- The local `gh` token lacks the `workflow` scope; workflow-file changes
  are pushed with git over SSH, which works.
