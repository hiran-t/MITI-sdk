# miti_bridge built from this checkout, in the same image as docker/Dockerfile: for trying a change
# to libmiti or miti_bridge in a Docker setup before it is released. It is built with colcon into
# /opt/miti_bridge, which docker/entrypoint.sh sources.
#
#   docker build -f docker/source.Dockerfile --build-arg ROS_DISTRO=jazzy -t miti-bridge:source .
#
# docker/source.Dockerfile.dockerignore keeps build output and local environments out of the build.
ARG ROS_DISTRO=jazzy
FROM ros:${ROS_DISTRO}-ros-base
ARG ROS_DISTRO
ARG DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

# The build tools and libraries of the README's source build, and the message packages of
# docker/Dockerfile.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ninja-build pkg-config python3-colcon-common-extensions \
      libboost-dev nlohmann-json3-dev libturbojpeg0-dev \
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
    && rm -rf /var/lib/apt/lists/*

COPY core /src/core
COPY ros/miti_bridge /src/ros/miti_bridge
RUN source "/opt/ros/${ROS_DISTRO}/setup.bash" \
 && cd /src \
 && colcon build --merge-install --install-base /opt/miti_bridge --base-paths core ros \
      --cmake-args -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DMITI_BUILD_TESTS=OFF -DMITI_BUILD_EXAMPLES=OFF \
 && rm -rf /src

LABEL org.opencontainers.image.title="miti_bridge (from source)" \
      org.opencontainers.image.description="Serves a ROS 2 robot's topics, services, parameters and package files to MITI" \
      org.opencontainers.image.source="https://github.com/hiran-t/MITI-sdk" \
      org.opencontainers.image.licenses="MIT"

COPY docker/entrypoint.sh /entrypoint.sh
EXPOSE 8765
ENTRYPOINT ["/entrypoint.sh"]
