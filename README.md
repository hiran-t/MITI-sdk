# MITI SDK

The robot side of [MITI](https://github.com/hiran-t/MITI). **miti_bridge** connects a ROS 2 robot to MITI and replaces foxglove_bridge. It is built on **libmiti**, a C++ and Python library that any program can use to stream to MITI, with or without ROS.

**Status: design.** Nothing is built yet. Start with:

- [docs/design.md](docs/design.md): goals, architecture, phases and decisions
- [docs/protocol.md](docs/protocol.md): the MITI protocol, version 1 (`miti.bridge.v1`)

| Part          | What it is                                                                                                                         |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `libmiti`     | The C++ core, without ROS: WebSocket server, priority scheduling, JPEG and point cloud reduction, typed messages for MITI's panels |
| Python        | Bindings for `libmiti`, with numpy arrays passed without copies                                                                    |
| `miti_bridge` | A ROS 2 node (Humble, Jazzy) on `libmiti`, which serves a robot's topics, services, parameters and meshes to MITI                  |

The bridge sends less over slow links, decided on the robot before data leaves it: rate limits, newest-frame delivery for cameras and point clouds, priority for TF, JPEG for raw images and point cloud reduction.

Panels for MITI are built with the widget SDK, [MITI-widget-sdk](https://github.com/hiran-t/MITI-widget-sdk), which was in this repository until 2026-11-02.

## License

[MIT](LICENSE).
