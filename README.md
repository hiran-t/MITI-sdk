# MITI SDK

The robot side of [MITI](https://github.com/hiran-t/MITI). **miti_bridge** connects a ROS 2 robot to MITI and replaces foxglove_bridge. It is built on **libmiti**, a C++ and Python library that any program can use to stream to MITI, with or without ROS.

**Status: phase 1 done: libmiti's core works; miti_bridge is not started.** libmiti passes the conformance suite, as foxglove_bridge does. Start with:

- [docs/design.md](docs/design.md): goals, architecture, phases and decisions
- [docs/protocol.md](docs/protocol.md): the MITI protocol, version 1 (`miti.bridge.v1`)
- [conformance/](conformance/README.md): checks a server against the protocol, on a fixture ROS 2 graph
- [core/](core/README.md): libmiti, the C++ core
- [prototypes/generic_service_client/](prototypes/generic_service_client/README.md): calling a service of any type on Humble, which miti_bridge needs

| Part          | What it is                                                                                                                         |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `libmiti`     | The C++ core, without ROS: WebSocket server, priority scheduling, JPEG and point cloud reduction, typed messages for MITI's panels |
| Python        | Bindings for `libmiti`, with numpy arrays passed without copies                                                                    |
| `miti_bridge` | A ROS 2 node (Humble, Jazzy) on `libmiti`, which serves a robot's topics, services, parameters and meshes to MITI                  |

The bridge sends less over slow links, decided on the robot before data leaves it: rate limits, newest-frame delivery for cameras and point clouds, priority for TF, JPEG for raw images and point cloud reduction.

Panels for MITI are built with the widget SDK, [MITI-widget-sdk](https://github.com/hiran-t/MITI-widget-sdk), which was in this repository until 2026-11-02.

## Develop

[pixi](https://pixi.sh) brings ROS 2, foxglove_bridge, a C++ toolchain and Node.js, so no system ROS install is needed (Linux and macOS arm64):

```bash
pixi run -e humble conformance-foxglove   # the conformance suite against foxglove_bridge (-e jazzy too)
pixi run -e humble service-prototype      # build the service-client prototype and call the fixture
pixi run -e core test                     # build libmiti and run its tests (no ROS needed)
pixi run -e core conformance-libmiti      # the conformance suite against libmiti's example server
pixi run type-check                       # type-check the conformance suite
```

CI (`.github/workflows/ci.yml`) runs the same, on every supported Ubuntu (22.04 and 24.04) and on both amd64 and arm64:

- libmiti on each of the four, and once more with sanitizers.
- The ROS checks in `ros:<distro>-ros-base` containers: Humble on 22.04, Jazzy on 24.04, each on both architectures.

## License

[MIT](LICENSE).
