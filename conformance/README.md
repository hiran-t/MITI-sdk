# Conformance suite

Checks a server against [the MITI protocol](../docs/protocol.md). It runs against a fixture ROS 2 graph whose topics, service, parameter and mesh it knows.

- **Base protocol** (`test/base.test.ts`): every server MITI talks to must pass these: libmiti servers, and foxglove_bridge until MITI drops it.
- **Extensions** (`test/extensions.test.ts`): only for servers that select `miti.bridge.v1`. They are skipped otherwise, and are to-dos until libmiti implements them.

## Run

With pixi, from the repository root:

```bash
pixi run -e humble conformance-foxglove
```

```bash
pixi run -e jazzy conformance-foxglove
```

With ROS 2 sourced from `/opt/ros` instead, run `npm ci` in `conformance/` first, then:

```bash
bash conformance/run.sh foxglove
```

```bash
bash conformance/run.sh url ws://127.0.0.1:8765
```

- `foxglove` starts the fixture and foxglove_bridge.
- `url` starts only the fixture, for a server you have already started on the same `ROS_DOMAIN_ID`.

**Isolation.** The fixture and server run on a random `ROS_DOMAIN_ID` from 100 to 199, reachable only from this machine. This keeps other robots on the network from disturbing the run, and keeps them from seeing it. Set `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION` (default `rmw_fastrtps_cpp`) or `CONFORMANCE_PORT` (default `18765`) to change that.

## Layout

| Path                      | What it is                                                                                                                                                                                                                         |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/protocol.ts`         | A client for the protocol, written from `docs/protocol.md`. It keeps every received message, so a check can find one that already arrived                                                                                          |
| `src/cdr.ts`              | Just enough little-endian CDR for the fixture's messages                                                                                                                                                                           |
| `src/fixture.ts`          | The fixture's names and values, and `connectWhenReady`, which waits until the server listens and offers the fixture's topics. Discovery takes seconds, and longer on Jazzy                                                         |
| `fixture/fixture_node.py` | The fixture graph: `/conformance/string`, an image, a point cloud, `/tf`, `/tf_static`, `/clock`, an echo from `/conformance/in` to `/conformance/echo`, the service `/conformance/set_bool` and the parameter `robot_description` |
| `fixture/share/`          | `fixture/` is an install prefix of its own. Its `miti_conformance_fixture` package holds `meshes/cube.stl`, which `package://` URIs resolve to once `fixture/` is on `AMENT_PREFIX_PATH`                                           |

## Results

foxglove_bridge 3.6 passes every base check on Humble and Jazzy: 18 pass, and 1 is skipped because it is an extension.

- **Locally:** pixi on macOS arm64.
- **CI:** `ros:<distro>-ros-base` on amd64 and arm64, with the bridge from apt (3.6.0).

Things this confirmed about foxglove_bridge, which docs/protocol.md now requires of every server:

- It answers a subscription to an unknown channel with a `status` error.
- It answers a call to an unknown service with `serviceCallFailure`.
- It answers an asset outside its allowlist with a failed `fetchAssetResponse`.
