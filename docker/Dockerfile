# miti_bridge as a service: ROS 2 with miti_bridge from the MITI SDK's .debs and the message
# packages a robot usually publishes. Run it with host networking, as docker/docker-compose.yml does,
# so DDS finds the robot's nodes. Published as ghcr.io/hiran-t/miti-bridge:<version>-<distro>.
#
# The .debs come from dist/, for the distro and each architecture built:
#
#   gh release download v0.1.2 --repo hiran-t/MITI-sdk --pattern 'ros-jazzy-*' --dir dist
#   docker build -f docker/Dockerfile --build-arg ROS_DISTRO=jazzy -t miti-bridge:jazzy .
ARG ROS_DISTRO=jazzy
FROM ros:${ROS_DISTRO}-ros-base
ARG ROS_DISTRO
ARG TARGETARCH

COPY dist/ros-${ROS_DISTRO}-*_${TARGETARCH}.deb /tmp/debs/

# Message definitions the bridge needs to offer the usual robot topics. The robot's own packages
# come from a workspace mounted at /overlay (see docker/entrypoint.sh).
RUN apt-get update && apt-get install -y --no-install-recommends \
      /tmp/debs/*.deb \
      ros-${ROS_DISTRO}-rmw-cyclonedds-cpp \
      ros-${ROS_DISTRO}-common-interfaces \
      ros-${ROS_DISTRO}-tf2-msgs \
      ros-${ROS_DISTRO}-nav2-msgs \
      ros-${ROS_DISTRO}-map-msgs \
      ros-${ROS_DISTRO}-moveit-msgs \
      ros-${ROS_DISTRO}-foxglove-msgs \
      ros-${ROS_DISTRO}-control-msgs \
      ros-${ROS_DISTRO}-vision-msgs \
      ros-${ROS_DISTRO}-bond \
    && rm -rf /var/lib/apt/lists/* /tmp/debs

LABEL org.opencontainers.image.title="miti_bridge" \
      org.opencontainers.image.description="Serves a ROS 2 robot's topics, services, parameters and package files to MITI" \
      org.opencontainers.image.source="https://github.com/hiran-t/MITI-sdk" \
      org.opencontainers.image.licenses="MIT"

COPY docker/entrypoint.sh /entrypoint.sh
EXPOSE 8765
ENTRYPOINT ["/entrypoint.sh"]
