#!/bin/bash
# Starts miti_bridge with `ros2 launch miti_bridge miti_start`.
#
# Settings from the environment:
#   MITI_BRIDGE_PORT     WebSocket port (8765)
#   MITI_BRIDGE_ADDRESS  address to listen on (0.0.0.0)
#   ROS_DOMAIN_ID, RMW_IMPLEMENTATION, ROS_LOCALHOST_ONLY, ROS_AUTOMATIC_DISCOVERY_RANGE and the
#   DDS variables reach ROS 2 as they are.
# Other miti_start arguments follow the image name, for example
#   docker run ... ghcr.io/hiran-t/miti-bridge:jazzy "topic_whitelist:=['/tf.*', '/camera/.*']"
#
# The robot's own packages (message types, and package:// meshes) come from a colcon workspace
# built for the same ROS distro and mounted at /overlay: its install/setup.bash is sourced.
set -e
source "/opt/ros/${ROS_DISTRO}/setup.bash"
if [ -f /overlay/install/setup.bash ]; then
  source /overlay/install/setup.bash
  echo "miti_bridge: using the workspace in /overlay"
elif [ -f /overlay/setup.bash ]; then
  source /overlay/setup.bash
  echo "miti_bridge: using the workspace in /overlay"
fi
exec ros2 launch miti_bridge miti_start \
  "port:=${MITI_BRIDGE_PORT:-8765}" "address:=${MITI_BRIDGE_ADDRESS:-0.0.0.0}" "$@"
