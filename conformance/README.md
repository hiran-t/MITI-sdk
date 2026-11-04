# Conformance suite

Checks a server against [the MITI protocol](../docs/protocol.md). The checks know the fixture's topics, service, parameter and mesh. A ROS server serves them from the fixture graph (`fixture/`). libmiti's example server offers the same without ROS.

- **Base protocol** (`test/base.test.ts`): every server MITI talks to must pass these: libmiti servers, and foxglove_bridge until MITI drops it.
- **Extensions** (`test/extensions.test.ts`): only for servers that select `miti.bridge.v1`. They are skipped otherwise, and are to-dos until libmiti implements them.

## Run

With pixi, from the repository root:

```bash
pixi run -e core conformance-libmiti
```

```bash
pixi run -e humble conformance-foxglove
```

```bash
pixi run -e jazzy conformance-foxglove
```

```bash
pixi run -e humble conformance-bridge
```

On Windows, run these from Git Bash: `run.sh` is a bash script.

Without pixi, run `npm ci` in `conformance/` first, then (the ROS modes with ROS 2 sourced from `/opt/ros`):

```bash
bash conformance/run.sh libmiti core/build/examples/conformance_server
```

```bash
bash conformance/run.sh foxglove
```

```bash
bash conformance/run.sh url ws://127.0.0.1:8765
```

- `libmiti` starts the example server. It needs no ROS.
- `miti-bridge` starts the fixture and the miti_bridge binary given.
- `foxglove` starts the fixture and foxglove_bridge.
- `url` starts only the fixture, for a server you have already started on the same `ROS_DOMAIN_ID`.

**Isolation.** The fixture and server run on a random `ROS_DOMAIN_ID` from 100 to 199, reachable only from this machine. This keeps other robots on the network from disturbing the run, and keeps them from seeing it. Set `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION` (default `rmw_fastrtps_cpp`) or `CONFORMANCE_PORT` (default `18765`) to change that.

## Layout

| Path                      | What it is                                                                                                                                                                                                                                                   |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/protocol.ts`         | A client for the protocol, written from `docs/protocol.md`. It keeps every received message, so a check can find one that already arrived                                                                                                                    |
| `src/cdr.ts`              | Just enough little-endian CDR for the fixture's messages                                                                                                                                                                                                     |
| `src/fixture.ts`          | The fixture's names and values, and `connectWhenReady`, which waits until the server listens and offers the fixture's topics. Discovery takes seconds, and longer on Jazzy                                                                                   |
| `fixture/fixture_node.py` | The fixture graph: `/conformance/string`, an rgb8 image, a 16UC1 depth image, a point cloud, `/tf`, `/tf_static`, `/clock`, an echo from `/conformance/in` to `/conformance/echo`, the service `/conformance/set_bool` and the parameter `robot_description` |
| `fixture/share/`          | `fixture/` is an install prefix of its own. Its `miti_conformance_fixture` package holds `meshes/cube.stl`, which `package://` URIs resolve to once `fixture/` is on `AMENT_PREFIX_PATH`                                                                     |

## Results

libmiti's example server (`core/examples/conformance_server`) and miti_bridge (Humble and Jazzy) pass every check, on Ubuntu and on Windows: 33. One is skipped: it applies only to a server without `miti.transforms`. Two are to-dos, `latest` delivery and priorities, which a fast local connection cannot show; libmiti's scheduler tests check them, and `bench/link` over a slowed link.

foxglove_bridge 3.6 passes every base check on Humble and Jazzy: 19 pass, and the 15 extension checks are skipped.

- **Locally:** pixi on macOS arm64.
- **CI:** `ros:<distro>-ros-base` on amd64 and arm64, with the bridge from apt (3.6.0).
- **Windows x64,** with RoboStack's 3.6.0: it passed in four of five CI runs. In the other it did not see the fixture's topics within 30 s, so CI does not run it there.

Things this confirmed about foxglove_bridge, which docs/protocol.md now requires of every server:

- It answers a subscription to an unknown channel with a `status` error.
- It answers a call to an unknown service with `serviceCallFailure`.
- It answers an asset outside its allowlist with a failed `fetchAssetResponse`.
