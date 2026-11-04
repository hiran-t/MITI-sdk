#!/usr/bin/env bash
# The link benchmark (docs/design.md, "The link, measured"): a robot publishing a raw 1280x720 camera at 30 Hz
# and TF at 50 Hz, its bridge behind a 20 Mbit/s link with 30 ms of delay (tc netem on the robot's
# side), and a client that reports how late TF and camera frames arrive.
#   bench/link/run.sh miti-jpeg|miti-raw|reference [camera fps] [seconds]
# The client asks miti_bridge for at most `camera fps` (default 15, as MITI's Image panel; 0 for
# every frame).
# LINK sets the netem link instead (`LINK=none` for none).
# Runs on Humble in Docker. The miti modes need the miti_bridge .deb in dist/
# (packaging/build-debs.sh). The client is Node.js 22.18+ in Docker.
set -euo pipefail

mode="${1:?miti-jpeg, miti-raw or reference}"
fps="${2:-15}"
seconds="${3:-20}"
link="${LINK:-rate 20mbit delay 30ms}"
[ "$link" = none ] && shape=true || shape="tc qdisc add dev eth0 root netem $link"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
network=miti-link-bench
robot=miti-link-robot

case "$mode" in
  miti-jpeg | miti-raw)
    packages="/src/dist/ros-humble-miti-bridge_*.deb"
    bridge="ros2 run miti_bridge miti_bridge"
    ;;
  reference)
    packages="ros-humble-foxglove-bridge"
    bridge="ros2 run foxglove_bridge foxglove_bridge"
    ;;
  *)
    echo "Unknown mode $mode" >&2
    exit 2
    ;;
esac
camera=raw
[ "$mode" = miti-jpeg ] && camera=jpeg

docker network create "$network" >/dev/null 2>&1 || true
docker rm -f "$robot" >/dev/null 2>&1 || true
trap 'docker rm -f "$robot" >/dev/null 2>&1 || true' EXIT

# DDS stays inside the robot container; only the bridge's WebSocket crosses the shaped link.
# Fast DDS sends the camera through shared memory, which needs more than Docker's 64 MB default.
docker run -d --name "$robot" --network "$network" --cap-add NET_ADMIN --shm-size 1g \
  -v "$root":/src:ro ros:humble-ros-base bash -c "
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq iproute2 python3-numpy ros-humble-sensor-msgs ros-humble-tf2-msgs \
      $packages >/dev/null
    source /opt/ros/humble/setup.bash
    export ROS_LOCALHOST_ONLY=1
    $shape
    python3 /src/bench/link/camera_node.py &
    exec $bridge" >/dev/null

docker run --rm --network "$network" -v "$root":/src:ro node:22-bookworm \
  node /src/bench/link/measure.mts "ws://$robot:8765" "$camera" "$fps" "$seconds"
