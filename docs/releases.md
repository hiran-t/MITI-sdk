# Releasing

A release publishes `ros-<distro>-miti-bridge` and `ros-<distro>-libmiti`:

- **Distros and architectures:** Humble (Ubuntu 22.04) and Jazzy (Ubuntu 24.04), each on amd64 and arm64.
- **Where:** attached to the GitHub release, and in the apt repository at `https://hiran-t.github.io/MITI-sdk`.

## Versions

One version covers the whole repository. It is written in three places:

- `core/package.xml`
- `core/CMakeLists.txt` (`project(libmiti VERSION …)`)
- `ros/miti_bridge/package.xml`

The release tag is `v` and that version, for example `v0.1.0`. The Packages workflow refuses a tag that does not match all three.

0.x versions may change libmiti's API from one minor version to the next, but not within a ROS distro it has been released into ([The ROS buildfarm](#the-ros-buildfarm)). 1.0 follows [the design](design.md).

## Steps

1. **Set the version.** Put the new version in the three files, and add a section for it at the top of `core/CHANGELOG.rst` and `ros/miti_bridge/CHANGELOG.rst` ([REP 132](https://reps.openrobotics.org/rep-0132/) format, which bloom reads). Commit and push to `main`, and wait for CI and Packages to pass.
2. **Publish the release:**

   ```bash
   gh release create v0.1.0 --repo hiran-t/MITI-sdk --title v0.1.0 --notes "…"
   ```

3. **Let the Packages workflow finish.** For the published release, it:
   - builds the packages from the tag on all four combinations
   - installs each in a fresh container, starts it with `ros2 launch miti_bridge miti_start`, and runs the conformance suite
   - attaches the packages to the release
   - rebuilds the apt repository from the packages of the last three releases, and force-pushes it to the `gh-pages` branch

To publish an existing tag again, run the workflow by hand with that tag (Actions → Packages → Run workflow).

## Set up once

**The signing key.** The apt repository is signed with the secret `APT_GPG_PRIVATE_KEY`, an ASCII-armored private key without a passphrase. Without it, the workflow attaches the packages and skips the repository.

The key, created 2026-11-02:

- **Name:** "MITI SDK apt repository"
- **Type:** RSA 4096, like MITI's, with no expiry
- **Fingerprint:** `8DD6 9B84 2E59 9DAA 8E07 D0A5 96C5 DC6F D235 6237`

Users trust it once, and changing it later means every user adds the new `key.gpg` again. Keep its backup offline.

To set the secret from the backup:

```bash
gh secret set APT_GPG_PRIVATE_KEY --repo hiran-t/MITI-sdk < miti-sdk-apt-key.asc
```

To replace it, create a new key on a trusted machine, then set the secret and keep a backup. On macOS, keep `GNUPGHOME` short: gpg-agent's socket path has a length limit.

```bash
export GNUPGHOME="$HOME/.gnupg-new-key" && mkdir -m 700 "$GNUPGHOME"
gpg --batch --passphrase '' --quick-gen-key "MITI SDK apt repository" rsa4096 sign never
gpg --armor --export-secret-keys "MITI SDK apt repository" > miti-sdk-apt-key.asc   # back it up offline
gh secret set APT_GPG_PRIVATE_KEY --repo hiran-t/MITI-sdk < miti-sdk-apt-key.asc
gpgconf --kill gpg-agent && rm -rf "$GNUPGHOME"
```

**GitHub Pages.** After the first publish, serve the `gh-pages` branch: Settings → Pages → Deploy from a branch → `gh-pages`, `/ (root)`.

## The apt repository

- **Suites:** one per Ubuntu release, as in ROS's own repository: `jammy` holds the Humble packages, `noble` the Jazzy ones. Users pick theirs with `$UBUNTU_CODENAME`; see the [README](../README.md#install-miti_bridge).
- **How it is built:** `packaging/build-apt-repo.sh`. `packaging/build-debs.sh` builds the packages with bloom, and `packaging/test-deb.sh` checks one the way a user installs it.

## The ROS buildfarm

Releasing through the ROS buildfarm puts `ros-$ROS_DISTRO-miti-bridge` and `ros-$ROS_DISTRO-libmiti` in ROS's own repositories (`packages.ros.org`), with no repository of ours to add.

**Platforms.** The buildfarm releases binaries for these (`ros2/ros_buildfarm_config`, `*/release-*build.yaml`):

| Distro | Ubuntu                 | RHEL      |
| ------ | ---------------------- | --------- |
| Humble | jammy, amd64 and arm64 | 8, x86_64 |
| Jazzy  | noble, amd64 and arm64 | 9, x86_64 |

CI builds and tests all of them except RHEL 8, which has Boost 1.66 and nlohmann_json 3.6.1, older than libmiti needs. Humble on RHEL 8 is skipped through `package_blacklist` in `humble/release-rhel-build.yaml` of `ros2/ros_buildfarm_config`.

**Packages.** Only libmiti (`core/`) and miti_bridge (`ros/miti_bridge/`) are packages: `conformance/` and `prototypes/` carry `COLCON_IGNORE` and `CATKIN_IGNORE`, so colcon, bloom and the buildfarm skip them.

**API.** Once a version is in a ROS distro, later releases into that distro keep libmiti's API and ABI.

**The first release:**

1. A pull request to `ros/rosdistro` adding `doc` and `source` entries for this repository to `humble/distribution.yaml` and `jazzy/distribution.yaml`, with `test_pull_requests: true`. Its review checks the package names ([REP 144](https://reps.openrobotics.org/rep-0144/)) and the licenses.
2. Once it is merged, issues on `ros2-gbp/ros2-gbp-github-org` for a release team and a release repository.
3. `bloom-release --new-track --rosdistro humble --track humble <repository>`, then the same for Jazzy, with `v{version}` as the upstream tag. Bloom opens the `ros/rosdistro` pull request for the release.
4. After it is merged, the packages reach `ros-testing` within a day or two, and the main repository at the distro's next sync, every two to four weeks. The status pages at `repo.ros2.org/status_page/` show each build.

Later releases run `bloom-release --rosdistro <distro> <repository>` after the GitHub release.
