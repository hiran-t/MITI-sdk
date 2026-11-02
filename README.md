# MITI SDK

The robot side of [MITI](https://github.com/hiran-t/MITI). **miti_bridge** connects a ROS 2 robot to MITI and replaces foxglove_bridge. It is built on **libmiti**, a C++ and Python library that any program can use to stream to MITI, with or without ROS.

**Status: phase 0 done, libmiti not started.** The protocol is specified, and the conformance suite that will check libmiti already passes against foxglove_bridge. Start with:

- [docs/design.md](docs/design.md): goals, architecture, phases and decisions
- [docs/protocol.md](docs/protocol.md): the MITI protocol, version 1 (`miti.bridge.v1`)
- [conformance/](conformance/README.md): checks a server against the protocol, on a fixture ROS 2 graph
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
pixi run type-check                       # type-check the conformance suite
```

CI (`.github/workflows/ci.yml`) runs the same on Humble and Jazzy, in `ros:<distro>-ros-base` containers on amd64 and arm64.

## License

[MIT](LICENSE).
