#!/usr/bin/env bash
# Runs the conformance suite against a server:
#   conformance/run.sh libmiti core/build/examples/conformance_server
#       libmiti's example server, which offers what the fixture graph offers; needs no ROS
#   conformance/run.sh miti-bridge install/humble/lib/miti_bridge/miti_bridge
#       miti_bridge beside the fixture graph
#   conformance/run.sh foxglove
#       the reference bridge beside the fixture graph, for the base protocol
#   conformance/run.sh url ws://HOST:PORT
#       a server already running, with the fixture graph started here
# Needs Node.js 22.18+ with `npm ci` done in conformance/. The ROS modes also need ROS 2 (a pixi
# environment, or /opt/ros sourced) and python3 with rclpy.
#
# The fixture graph runs on its own ROS_DOMAIN_ID and on this machine only, so other robots on the
# network neither disturb it nor see it. Settings: ROS_DOMAIN_ID, RMW_IMPLEMENTATION,
# CONFORMANCE_PORT.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server="${1:-foxglove}"
port="${CONFORMANCE_PORT:-18765}"

pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
trap cleanup EXIT

start_fixture() {
  export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-$((100 + RANDOM % 100))}"
  export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
  if [ "${ROS_DISTRO:-}" = "humble" ]; then
    export ROS_LOCALHOST_ONLY=1
  else
    export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
  fi
  # The fixture's package (its mesh) is found through the ament index, by the server too.
  export AMENT_PREFIX_PATH="$here/fixture${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
  python3 "$here/fixture/fixture_node.py" &
  pids+=($!)
  echo "Fixture graph: ROS_DOMAIN_ID=$ROS_DOMAIN_ID, ${ROS_DISTRO:-ROS} with $RMW_IMPLEMENTATION"
}

case "$server" in
  libmiti)
    binary="${2:?conformance/run.sh libmiti PATH/TO/conformance_server}"
    "$binary" --port "$port" --share "$here/fixture/share" &
    pids+=($!)
    url="ws://127.0.0.1:$port"
    ;;
  miti-bridge)
    binary="${2:?conformance/run.sh miti-bridge PATH/TO/miti_bridge}"
    start_fixture
    "$binary" --ros-args -p port:="$port" -p address:=127.0.0.1 -p use_sim_time:=true &
    pids+=($!)
    url="ws://127.0.0.1:$port"
    ;;
  foxglove)
    start_fixture
    bridge=""
    for prefix in ${AMENT_PREFIX_PATH//:/ }; do
      [ -x "$prefix/lib/foxglove_bridge/foxglove_bridge" ] && bridge="$prefix/lib/foxglove_bridge/foxglove_bridge" && break
    done
    [ -n "$bridge" ] || { echo "foxglove_bridge not found on AMENT_PREFIX_PATH" >&2; exit 1; }
    "$bridge" --ros-args -p port:="$port" -p address:=127.0.0.1 -p use_sim_time:=true \
      --log-level foxglove_bridge:=warn &
    pids+=($!)
    url="ws://127.0.0.1:$port"
    ;;
  url)
    url="${2:?conformance/run.sh url ws://HOST:PORT}"
    start_fixture
    ;;
  *)
    echo "Unknown server $server: use libmiti, miti-bridge, foxglove, or url" >&2
    exit 2
    ;;
esac

echo "Conformance: $server at $url"
cd "$here"
MITI_SERVER_URL="$url" node --test --test-reporter=spec test/*.test.ts
