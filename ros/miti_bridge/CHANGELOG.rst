^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package miti_bridge
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.1.3 (2026-11-23)
------------------
* A transient-local publisher that starts after a client subscribed is delivered: the bridge
  re-advertises the channel as latched and subscribes again with its publishers' QoS. Before, a
  client that subscribed to ``/robot_description`` before the robot started never got it.
* Built on libmiti 0.1.3, which offers ``keepNewest`` for rate-limited subscriptions.
* A Docker image, ``ghcr.io/hiran-t/miti-bridge``, runs the bridge as a service; see
  ``docs/docker.md``.

0.1.2 (2026-11-04)
------------------
* Builds and runs on Windows x64 with ROS 2 Humble and Jazzy from RoboStack, from source with pixi.
* Built on libmiti 0.1.2, which refuses asset URIs that contain a NUL character.

0.1.1 (2026-11-03)
------------------
* Robot meshes from workspaces built with ``colcon build --symlink-install`` are served.

0.1.0 (2026-11-02)
------------------
* First release: serves a ROS 2 robot's topics, services, parameters, package files and clock to
  MITI over ``miti.bridge.v1``. Camera images go as JPEG, point clouds are reduced and rate limits
  apply on the robot, and the link's statistics are reported.
