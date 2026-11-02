#!/usr/bin/env bash
# Builds ros-<distro>-libmiti and ros-<distro>-miti-bridge .debs with bloom, into dist/:
#   packaging/build-debs.sh            (ROS_DISTRO from the environment)
# Runs as root in ros:<distro>-ros-base (CI does, and so can Docker locally), from the repository
# root. It installs build tools, and the libmiti .deb, because miti_bridge is built against it.
set -euo pipefail

distro="${ROS_DISTRO:?run in a ROS environment}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq --no-install-recommends python3-bloom fakeroot dpkg-dev debhelper >/dev/null

# bloom resolves every dependency through rosdep, libmiti included.
echo "yaml file://$root/packaging/rosdep.yaml" >/etc/ros/rosdep/sources.list.d/50-miti.list
rosdep update --rosdistro "$distro" >/dev/null

build() {
  local package_dir="$1"
  (
    cd "$package_dir"
    rm -rf debian obj-*
    rosdep install --from-paths . --ignore-src -y --rosdistro "$distro" >/dev/null
    bloom-generate rosdebian --ros-distro "$distro" >/dev/null
    fakeroot debian/rules binary
    rm -rf debian obj-*
  )
}

mkdir -p "$root/dist"
build "$root/core"
mv "$root"/ros-"$distro"-libmiti_*.deb "$root/dist/"
apt-get install -y -qq "$root"/dist/ros-"$distro"-libmiti_*.deb >/dev/null
build "$root/ros/miti_bridge"
mv "$root"/ros/ros-"$distro"-miti-bridge_*.deb "$root/dist/"
ls -l "$root/dist"
