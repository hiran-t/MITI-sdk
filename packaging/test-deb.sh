#!/usr/bin/env bash
# Installs the miti_bridge .deb from dist/ the way a user does, then checks it: it starts with
# `ros2 launch miti_bridge miti_start` and passes the conformance suite.
#   packaging/test-deb.sh            (ROS_DISTRO from the environment)
# Runs as root in a fresh ros:<distro>-ros-base, from the repository root, with Node.js 22.18+
# and `npm ci` done in conformance/.
set -eo pipefail

distro="${ROS_DISTRO:?run in a ROS environment}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
# The bridge, and what the fixture graph publishes.
apt-get install -y -qq "$root"/dist/ros-"$distro"-miti-bridge_*.deb \
  ros-"$distro"-std-srvs ros-"$distro"-sensor-msgs ros-"$distro"-tf2-msgs >/dev/null
if dpkg -s ros-"$distro"-libmiti >/dev/null 2>&1; then
  echo "ros-$distro-libmiti was installed with the bridge, which links it statically" >&2
  exit 1
fi
source /opt/ros/"$distro"/setup.bash
dpkg-query -W ros-"$distro"-miti-bridge

export ROS_DOMAIN_ID=$((100 + RANDOM % 100)) RMW_IMPLEMENTATION=rmw_fastrtps_cpp
if [ "$distro" = humble ]; then
  export ROS_LOCALHOST_ONLY=1
else
  export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
fi
# The fixture's package (its mesh) must be known to the bridge when it starts.
export AMENT_PREFIX_PATH="$root/conformance/fixture:$AMENT_PREFIX_PATH"

port=18765
ros2 launch miti_bridge miti_start port:=$port address:=127.0.0.1 use_sim_time:=true &
launch=$!
# ros2 launch leaves its node running when it is itself terminated: stop both.
trap 'pkill -TERM -P $launch; kill -TERM $launch; wait $launch 2>/dev/null || true' EXIT
bash "$root/conformance/run.sh" url "ws://127.0.0.1:$port"
