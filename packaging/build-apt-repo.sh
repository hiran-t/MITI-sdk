#!/usr/bin/env bash
# Builds a signed apt repository from .deb files, one suite per Ubuntu release as ROS's own
# repository does (jammy for Humble, noble for Jazzy), component "main", amd64 and arm64:
#   packaging/build-apt-repo.sh <output dir> <gpg key id> dist/*.deb
# Needs apt-ftparchive (apt-utils) and gpg with the private key. Users add it with:
#   curl -fsSL <url>/key.gpg | sudo gpg --dearmor -o /usr/share/keyrings/miti-sdk.gpg
#   echo "deb [signed-by=/usr/share/keyrings/miti-sdk.gpg] <url> $(. /etc/os-release && echo $UBUNTU_CODENAME) main" |
#     sudo tee /etc/apt/sources.list.d/miti-sdk.list
set -euo pipefail

out="${1:?usage: packaging/build-apt-repo.sh <output dir> <gpg key id> <deb>...}"
key="${2:?gpg key id}"
shift 2
[ $# -gt 0 ] || { echo "No .deb files given" >&2; exit 1; }

rm -rf "$out"
for suite in jammy noble; do mkdir -p "$out/pool/main/$suite"; done
for deb in "$@"; do
  # bloom versions end in the Ubuntu codename: 0.1.0-0jammy.
  case "$(dpkg-deb -f "$deb" Version)" in
    *jammy) cp "$deb" "$out/pool/main/jammy/" ;;
    *noble) cp "$deb" "$out/pool/main/noble/" ;;
    *) echo "Not built for jammy or noble: $deb" >&2; exit 1 ;;
  esac
done
cd "$out"

for suite in jammy noble; do
  for arch in amd64 arm64; do
    dir="dists/$suite/main/binary-$arch"
    mkdir -p "$dir"
    apt-ftparchive --arch "$arch" packages "pool/main/$suite" >"$dir/Packages"
    gzip -9kn "$dir/Packages"
  done
  apt-ftparchive \
    -o APT::FTPArchive::Release::Origin="MITI SDK" \
    -o APT::FTPArchive::Release::Label="MITI SDK" \
    -o APT::FTPArchive::Release::Suite="$suite" \
    -o APT::FTPArchive::Release::Codename="$suite" \
    -o APT::FTPArchive::Release::Architectures="amd64 arm64" \
    -o APT::FTPArchive::Release::Components=main \
    -o APT::FTPArchive::Release::Description="miti_bridge and libmiti for ROS 2" \
    release "dists/$suite" >"dists/$suite/Release"
  gpg --batch --yes --local-user "$key" --clearsign -o "dists/$suite/InRelease" "dists/$suite/Release"
  gpg --batch --yes --local-user "$key" --armor --detach-sign -o "dists/$suite/Release.gpg" \
    "dists/$suite/Release"
done
gpg --armor --export "$key" >key.gpg

# GitHub Pages: serve files as they are, and say how to use the repository.
touch .nojekyll
cat >index.html <<'HTML'
<!doctype html>
<meta charset="utf-8">
<title>MITI SDK apt repository</title>
<h1>MITI SDK apt repository</h1>
<p>miti_bridge for ROS 2 Humble (Ubuntu 22.04) and Jazzy (Ubuntu 24.04), amd64 and arm64:</p>
<pre>
curl -fsSL https://hiran-t.github.io/MITI-sdk/key.gpg | sudo gpg --dearmor -o /usr/share/keyrings/miti-sdk.gpg
echo "deb [signed-by=/usr/share/keyrings/miti-sdk.gpg] https://hiran-t.github.io/MITI-sdk $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | sudo tee /etc/apt/sources.list.d/miti-sdk.list
sudo apt-get update
sudo apt-get install ros-$ROS_DISTRO-miti-bridge
ros2 launch miti_bridge miti_start
</pre>
<p>See <a href="https://github.com/hiran-t/MITI-sdk">github.com/hiran-t/MITI-sdk</a>.</p>
HTML
find . -type f | sort
