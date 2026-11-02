# MITI SDK

The robot side of [MITI](https://github.com/hiran-t/MITI). **miti_bridge** connects a ROS 2 robot to MITI and replaces foxglove_bridge. It is built on **libmiti**, a C++ and Python library that any program can use to stream to MITI, with or without ROS.

**Status: phase 3 done.** libmiti, and miti_bridge on Humble and Jazzy, pass the conformance suite. MITI connects to miti_bridge from its release after 3.4.2, and has it send cameras as JPEG and reduce point clouds on the robot. Typed messages and the Python binding are next. Start with:

- [docs/design.md](docs/design.md): goals, architecture, phases and decisions
- [docs/protocol.md](docs/protocol.md): the MITI protocol, version 1 (`miti.bridge.v1`)
- [conformance/](conformance/README.md): checks a server against the protocol, on a fixture ROS 2 graph
- [core/](core/README.md): libmiti, the C++ core
- [ros/miti_bridge/](ros/miti_bridge/README.md): the ROS 2 node, its parameters and how it works
- [prototypes/generic_service_client/](prototypes/generic_service_client/README.md): calling a service of any type on Humble, which miti_bridge needs
- [bench/link/](bench/link/run.sh): how late TF and a camera arrive over a slow link (`tc netem`), with miti_bridge or foxglove_bridge

| Part          | What it is                                                                                                                         |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `libmiti`     | The C++ core, without ROS: WebSocket server, priority scheduling, JPEG and point cloud reduction, typed messages for MITI's panels |
| Python        | Bindings for `libmiti`, with numpy arrays passed without copies                                                                    |
| `miti_bridge` | A ROS 2 node (Humble, Jazzy) on `libmiti`, which serves a robot's topics, services, parameters and meshes to MITI                  |

The bridge sends less over slow links, decided on the robot before data leaves it: rate limits, newest-frame delivery for cameras and point clouds, priority for TF, JPEG for raw images and point cloud reduction.

Panels for MITI are built with the widget SDK, [MITI-widget-sdk](https://github.com/hiran-t/MITI-widget-sdk), which was in this repository until 2026-11-02.

## Install and run

From this repository's apt repository (Humble on Ubuntu 22.04, Jazzy on 24.04, amd64 and arm64), after its first release. The steps are in the [miti_bridge README](ros/miti_bridge/README.md#install):

```bash
sudo apt install ros-$ROS_DISTRO-miti-bridge
```

```bash
ros2 launch miti_bridge miti_start
```

Releases are described in [docs/releases.md](docs/releases.md).

## Develop

[pixi](https://pixi.sh) brings ROS 2, foxglove_bridge, a C++ toolchain and Node.js, so no system ROS install is needed (Linux and macOS arm64):

```bash
pixi run -e humble conformance-foxglove   # the conformance suite against foxglove_bridge (-e jazzy too)
pixi run -e humble service-prototype      # build the service-client prototype and call the fixture
pixi run -e core test                     # build libmiti and run its tests (no ROS needed)
pixi run -e core conformance-libmiti      # the conformance suite against libmiti's example server
pixi run -e humble build-bridge           # libmiti and miti_bridge with colcon, into install/humble (-e jazzy too)
pixi run -e humble test-bridge            # their tests
pixi run -e humble conformance-bridge     # the conformance suite against miti_bridge and the fixture graph
pixi run type-check                       # type-check the conformance suite
```

CI (`.github/workflows/ci.yml`) runs the same, on every supported Ubuntu (22.04 and 24.04) and on both amd64 and arm64:

- libmiti on each of the four, and once more with sanitizers.
- The ROS checks in `ros:<distro>-ros-base` containers: Humble on 22.04, Jazzy on 24.04, each on both architectures.

## License

[MIT](LICENSE).
