#!/usr/bin/env bash
# Builds the prototype for this environment's ROS_DISTRO, starts the conformance fixture on an
# isolated domain and calls its std_srvs/srv/SetBool service with data=true. Prints the response
# CDR and fails unless it is success=true, message "set True".
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$here/build-${ROS_DISTRO:?run in a ROS environment}"

# A pixi environment is its own prefix; a sourced /opt/ros sets CMAKE_PREFIX_PATH itself.
cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release ${CONDA_PREFIX:+-DCMAKE_PREFIX_PATH="$CONDA_PREFIX"} >/dev/null
cmake --build "$build"

export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-$((100 + RANDOM % 100))}"
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
if [ "$ROS_DISTRO" = "humble" ]; then export ROS_LOCALHOST_ONLY=1; else export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST; fi

python3 "$here/../../conformance/fixture/fixture_node.py" &
fixture=$!
trap 'kill $fixture 2>/dev/null || true; wait 2>/dev/null || true' EXIT

# CDR: encapsulation header 00 01 00 00 (little-endian), then bool data = 01.
response="$("$build/generic_service_call" /conformance/set_bool std_srvs/srv/SetBool 0001000001)"
echo "response CDR: $response"
# Header, success = 01, 3 bytes of padding (any value: CDR does not zero them), string length 9,
# "set True", NUL.
expected='^0001000001[0-9a-f]{6}09000000736574205472756500'
[[ "$response" =~ $expected ]] || { echo "Expected a response matching $expected" >&2; exit 1; }
echo "OK on $ROS_DISTRO: success=true, message \"set True\""
