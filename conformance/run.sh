#!/usr/bin/env bash
# Runs the conformance suite against a server:
#   conformance/run.sh libmiti core/build/examples/conformance_server
#       libmiti's example server, which offers what the fixture graph offers; needs no ROS
#   conformance/run.sh miti-bridge install/humble/lib/miti_bridge/miti_bridge
#       miti_bridge beside the fixture graph
#   conformance/run.sh foxglove
#       foxglove_bridge beside the fixture graph, the reference for the base protocol
#   conformance/run.sh url ws://HOST:PORT
#       a server already running, with the fixture graph started here
# Needs Node.js 22.18+ with `npm ci` done in conformance/. The ROS modes also need ROS 2 (a pixi
# environment, or /opt/ros sourced) and python3 with rclpy. On Windows, run it from Git Bash.
#
# The fixture graph runs on its own ROS_DOMAIN_ID and on this machine only, so other robots on the
# network neither disturb it nor see it. Settings: ROS_DOMAIN_ID, RMW_IMPLEMENTATION,
# CONFORMANCE_PORT.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server="${1:-foxglove}"
port="${CONFORMANCE_PORT:-18765}"

# Windows (Git Bash): ROS takes Windows paths separated by ";", and conda's Python is python.exe.
python=python3
separator=:
native() { printf '%s' "$1"; }
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    python=python
    separator=';'
    native() { cygpath -w "$1"; }
    ;;
esac

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
  local fixture
  fixture="$(native "$here/fixture")"
  export AMENT_PREFIX_PATH="$fixture${AMENT_PREFIX_PATH:+$separator$AMENT_PREFIX_PATH}"
  "$python" "$here/fixture/fixture_node.py" &
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
    IFS="$separator" read -r -a prefixes <<< "$AMENT_PREFIX_PATH"
    for prefix in "${prefixes[@]}"; do
      for candidate in "$prefix/lib/foxglove_bridge/foxglove_bridge"{,.exe}; do
        [ -f "$candidate" ] && [ -x "$candidate" ] && bridge="$candidate" && break 2
      done
    done
    [ -n "$bridge" ] || { echo "foxglove_bridge not found on AMENT_PREFIX_PATH" >&2; exit 1; }
    # RoboStack on Windows installs the reference bridge's DLLs in Library\lib, off the DLL path.
    if [ "$separator" = ';' ] && [ -n "${CONDA_PREFIX:-}" ]; then
      PATH="$(cygpath -u "$CONDA_PREFIX")/Library/lib:$PATH"
    fi
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
