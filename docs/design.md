# MITI bridge: design

Status: draft, 2026-11-02.

**Purpose.** miti_bridge replaces foxglove_bridge in MITI completely. It is built on a library
of our own, `libmiti`, with C++ and Python APIs, the way foxglove_bridge is built on the Foxglove
SDK.

**Where it lives.** In the `MITI-sdk` repository, which holds the robot side of MITI. It is
public, under the MIT license. The widget SDK, for the browser side, is in `MITI-widget-sdk`.

**Related.** The remaining questions are in [Open decisions](#open-decisions), and the wire format
is in [protocol.md](protocol.md). Paths such as `src/lib/bridge/client.ts` or
`scripts/heavy-bridge.js` refer to the [MITI](https://github.com/hiran-t/MITI) repository.

## Why

MITI reaches robots only through foxglove_bridge. That causes two problems.

**Dependence.** The program that runs on the robot is not ours. Released foxglove_bridge versions
stay MIT, but Foxglove decides what happens next: license, protocol, which features survive. They
have already closed Studio's source, moved the bridge into `foxglove-sdk` and added a new
subprotocol (`foxglove.sdk.v1`). `pixi.toml` accepts any `foxglove-bridge >= 3.6`.

**The link.** In the Foxglove protocol, a client cannot ask for less data, so MITI works around it
in the browser (`src/lib/bridge/client.ts`):

- `maxRate` and `throttleMs` drop messages after they have already crossed the network.
- In `separate` stream mode, heavy topics get their own WebSockets so TF is not queued behind
  camera frames. Panels then drift apart in time.
- A raw 1280×720 rgb8 frame is 2.8 MB. On the robot's link that measured 1 fps, 2.6 s behind.
- Images and point clouds are decoded in full in Web Workers, even when a panel draws only part
  of them.

Only the sending side can fix this, and it has to run **on the robot**. A bridge beside MITI
subscribes over DDS, so raw data still crosses the link before the bridge can reduce it.

**Why a library under the bridge.** foxglove_bridge 3.6 is a thin ROS node
(`libfoxglove_bridge_component`) over a C++ SDK (`libfoxglove_cpp_shared`), which wraps a Rust
core (`libfoxglove`). We take the same shape for the same reasons:

- Programs without ROS can stream to MITI directly. Examples: Isaac Sim scripts, PLC and fieldbus
  gateways, vision applications, test rigs.
- The core is tested without DDS.
- Other middleware (ROS 1, Zenoh, MQTT) can be added later as adapters.

## Goals

1. **Replace foxglove_bridge.**
   - miti_bridge is the bridge MITI installs, starts and documents.
   - MITI's panels and `@miti/sdk` widgets keep working unchanged, including
     `MitiClient.hasCapability`.
   - Once robots run miti_bridge, MITI drops foxglove_bridge and `@foxglove/ws-protocol`.
2. **Fix the link.** On the robot, before data is sent: per-subscription rate limits, priority,
   newest-wins delivery for heavy topics, JPEG for raw images, point cloud reduction.
3. **`libmiti`, a C++ and Python library that serves MITI over WebSocket.**
   - miti_bridge is built on it, and so can any other program.
   - Programs use it to publish topics, provide services, parameters and assets, and receive what
     MITI publishes.
   - It includes typed messages for everything MITI's panels draw, so no ROS installation is needed.
   - The link fixes live in its core, so every program built on it gets them.
4. **Platforms.**
   - `libmiti`: Linux (amd64, arm64 including Jetson), macOS and Windows.
   - miti_bridge: Ubuntu 22.04/24.04 with Humble and Jazzy, and Windows x64 with RoboStack's Humble
     and Jazzy, built from source.

Not in the first release:

- ROS 1
- MCAP recording and playback
- A C ABI and bindings for other languages (C#, Rust)
- `setParameters`, parameter subscriptions and the connection graph. MITI does not use them.
- Authentication (see [Phases](#phases), Later).
- Clients other than MITI, such as the Foxglove app or Lichtblick.

## Approach: MITI's own protocol, framed like the Foxglove protocol

The MITI protocol (`miti.bridge.v1`) has two parts:

- **Base.** The operations MITI uses today, with the same JSON and binary framing as the part of
  Foxglove WebSocket protocol v1 that MITI uses.
- **Extensions.** MITI's flow control and transforms on top of the base.

The framing is kept the same for one reason: during the switch, MITI talks to robots that still
run foxglove_bridge and to robots that run miti_bridge. With the same framing, both go through one
client code path.

| Server          | Subprotocol it accepts                       | MITI gets                             |
| --------------- | -------------------------------------------- | ------------------------------------- |
| miti_bridge     | `miti.bridge.v1` only                        | Base plus extensions                  |
| foxglove_bridge | `foxglove.sdk.v1` or `foxglove.websocket.v1` | Today's behavior, until MITI drops it |

miti_bridge does not offer Foxglove's subprotocols, and does not present itself as a Foxglove
server.

Write `libmiti` from protocol.md, not from foxglove_bridge's or the Foxglove SDK's source. If any
part is ever adapted, keep its MIT notice.

### Base: what MITI uses

| Feature    | Operations                                                                                   | MITI uses it for                                     |
| ---------- | -------------------------------------------------------------------------------------------- | ---------------------------------------------------- |
| Topics     | `serverInfo`, `advertise`/`unadvertise`, `subscribe`/`unsubscribe`, `messageData`            | Every panel                                          |
| Status     | `status`                                                                                     | Errors in the connection status                      |
| Time       | `time`                                                                                       | `getServerTime()`: camera latency on sim time        |
| Publish    | client `advertise`, `clientMessageData` (cdr)                                                | 3D Scene Publish tool, Button/Switch widget          |
| Services   | `advertiseServices`, `serviceCallRequest` (cdr), `serviceCallResponse`, `serviceCallFailure` | Button/Switch widget                                 |
| Parameters | `getParameters`, `parameterValues`                                                           | URDF from `/robot_state_publisher.robot_description` |
| Assets     | `fetchAsset`, `fetchAssetResponse`                                                           | URDF files and `package://` meshes                   |

`serverInfo.capabilities` lists only what the program has enabled.

### Extensions

Extensions are JSON operations whose names start with `miti.`.

1. **`miti.subscriptionOptions`** (client → server)
   - Fields: `{ op, subscriptionId, maxRate?, delivery?: 'all' | 'latest', priority?: 'high' | 'normal' | 'low', transform? }`.
   - The client sends it after `subscribe`, and can send it again to change the options.
   - Defaults depend on the message type:
     - TF, joint states and odometry are `high` and `all`.
     - The heavy types in `client.ts`'s `HEAVY_TYPES` are `low` and `latest`.
     - The exception is `foxglove_msgs/CompressedVideo`, which stays `all`: dropping a video frame
       breaks the frames that depend on it.
2. **`miti.subscriptionFormat`** (server → client)
   - Fields: `{ op, subscriptionId, schemaName, schemaEncoding, schema }`.
   - The server sends it before the first message whenever a transform changes the message type,
     for example from Image to CompressedImage. The client decodes that subscription with this
     schema instead of the channel's.
3. **Transforms**
   - `{ kind: 'jpeg', quality, maxWidth? }`: turns raw `rgb8`, `bgr8`, `rgba8`, `bgra8` and `mono8`
     images into `sensor_msgs/msg/CompressedImage`. Depth images (`16UC1`, `32FC1`) stay raw in the
     first release, because MITI does not decode compressed depth yet.
   - `{ kind: 'reduce', voxel?, stride?, maxPoints?, fields? }`: sends a PointCloud2 with only the
     points and fields that are kept.
4. **`miti.getSchema`** → **`miti.schema`**
   - Fields: `{ type }`.
   - Returns a type's full ros2msg definition, so MITI can CDR-encode what it publishes on topics
     the robot does not advertise yet.
5. **`miti.subscribeStats`** → **`miti.stats`**
   - Sent at 1 Hz.
   - Contents: for each subscription, bytes/s, messages sent and dropped, and the age of the oldest
     queued message; for the client, its buffered bytes.
   - MITI uses it for a link health indicator.

## Architecture

```
 Programs                     Bindings          Core: libmiti (C++17, no ROS)                Browsers
 ────────                     ────────          ─────────────────────────────                ────────
 miti_bridge (ROS 2 node) ─┐
 your C++ program ─────────┼──► C++ API ──┐
 your Python program ──────┴─► Python API ┴─► channels ─► transforms ─► sessions ─► WebSocket ─► MITI
 (Isaac Sim, gateway, …)      (nanobind)       │            (rate, jpeg,   (priority,
                                               │             reduce)        backpressure)
                                               ├─ typed messages + CDR (MITI's panel types)
                                               ├─ service, parameter, client-publish callbacks
                                               └─ assets: roots + allowlist, or a callback
```

### Core (`libmiti`)

The core is C++17 and depends on Boost.Beast, nlohmann/json and libjpeg-turbo.

**Server**

- WebSocket I/O on its own thread.
- Implements the base and the extensions.
- Optional TLS.

**Channels**

- A program adds channels and logs messages to them. Each message is one shared buffer, fanned
  out to every session without copies; each session writes its own frame header with
  scatter-gather writes.
- `on_subscribe` and `on_unsubscribe` callbacks let a program, such as the ROS bridge, produce
  data only while someone is watching.

**Transforms**

- Run on a thread pool, once per (channel, parameters), shared by every client that asks for the
  same.
- Newest wins: while a frame is being encoded, a newer frame replaces the one waiting. MITI's
  decode pool uses the same rule.
- `maxRate` is applied before the transform, so frames that would be dropped are never encoded.

**Sessions and scheduler.** Each client has three queues:

- **control:** JSON operations, service and parameter replies, time
- **high:** small topics with `all` delivery, in a bounded FIFO queue
- **low:** one newest-message slot per `latest` subscription, served round-robin so one camera
  cannot starve another

The session writes only while the socket's buffered bytes are under a small limit, about 256 KB.
foxglove_bridge instead queues up to its 10 MB limit, and that queue is what makes data seconds
old. With the scheduler, one connection keeps TF real-time. Asset downloads still use their own
connection, because one asset is one WebSocket message of several megabytes.

**Typed messages**

- C++ structs, and a CDR reader and writer, for the types MITI's panels draw:
  - `std_msgs`: `Header`, `String`, `Bool`
  - `sensor_msgs`: `Image`, `CompressedImage`, `CameraInfo`, `PointCloud2`, `LaserScan`, `JointState`
  - `geometry_msgs`: `TransformStamped`, `PoseStamped`, `PoseWithCovarianceStamped`, `PoseArray`, `PointStamped`
  - `tf2_msgs/TFMessage`
  - `nav_msgs`: `Odometry`, `Path`, `OccupancyGrid`
  - `visualization_msgs`: `Marker`, `MarkerArray`
  - `rcl_interfaces/Log`
- Each type carries its ROS name and ros2msg definition. MITI's panels pick topics by ROS type, so
  a program without ROS looks like a ROS robot to MITI.
- The structs are generated at build time from vendored `.msg` files (ROS `common_interfaces`,
  `rcl_interfaces`, `geometry2`), kept with their licenses.
- Transforms use the same reader for Image and PointCloud2.

**Callbacks**

- Services: a name, a type, request and response schemas, and a handler that returns the
  response, or an error, asynchronously.
- Parameters: get handler.
- Client publish: a handler that receives the client channel and the CDR bytes.

**Assets**

- Either a list of roots with an extension allowlist (`.urdf`, `.dae`, `.stl`, `.obj`, `.glb`,
  `.gltf`, `.mtl`, `.png`, `.jpg`) and a size limit, or a program-supplied callback.
- `..` cannot lead out of a package or a root.
- Real paths must stay inside a root, which also rejects symlink escapes. Inside a package,
  symlinks are followed, as foxglove_bridge does: `colcon build --symlink-install` installs every
  file of a package as a link into the source tree, so refusing them broke every such workspace
  (found on the Isaac Sim G1 in v0.1.0).
- This is the largest security surface: an endpoint that reads files on the robot.

**Time:** `broadcast_time(ns)`.

### C++ API (sketch)

```cpp
#include <miti/miti.hpp>

auto server = miti::Server::create({.name = "line-3", .port = 8765});

// Typed: MITI's panel types, no ROS needed.
auto camera = server->channel<miti::msgs::sensor::Image>("/camera/color/image_raw");
camera.log(image);                       // header.stamp is the message time

// Raw: any schema, as the ROS bridge uses it.
auto channel = server->add_channel({
  .topic = "/robot/status", .encoding = "cdr",
  .schema_name = "acme_msgs/msg/Status", .schema_encoding = "ros2msg", .schema = definition});
channel->log(bytes, stamp_ns);

server->on_subscribe([](const miti::Channel& ch) { /* start producing */ });
server->add_service({.name = "/reset", .type = "std_srvs/srv/Trigger", /* schemas */},
                    [](miti::ServiceRequest req) -> miti::Future<miti::Bytes> { /* … */ });
server->set_asset_roots({"/opt/robot/share"});
```

### Python API (sketch)

```python
import miti

server = miti.Server(name="isaac-sim", port=8765)
camera = server.image_channel("/camera/rgb")
camera.log(frame, encoding="rgb8", frame_id="camera")   # numpy array, no copy

tf = server.tf_channel("/tf")
tf.log([miti.Transform("map", "base_link", translation=(x, y, 0), rotation=q)])

@server.service("/reset", "std_srvs/srv/Trigger")
def reset(request):
    return {"success": True, "message": "reset"}
```

The binding uses nanobind. It releases the GIL while logging and while I/O runs, and passes numpy
arrays through the buffer protocol, without copies.

### miti_bridge (ROS 2 package on `libmiti`)

**Graph watcher**

- Watches the ROS graph through graph events, and polls every second as a fallback.
- Adds a raw channel for each allowed topic, and removes it when the topic disappears.
- Applies the topic allowlist and blocklist (regular expressions).

**Definitions**

- Resolves `pkg/msg/Type` to the concatenated `.msg` text through the ament index, recursively.
- Types that ship only as `.idl` are not advertised in the first release, because MITI decodes
  ros2msg only. Each one is logged once.

**Topics**

- One generic subscription per topic, created from `on_subscribe` and ended some time after
  `on_unsubscribe`.
- QoS follows the publishers: reliability, and `transient_local` for `/tf_static` and
  `/robot_description`.
- The serialized bytes go to `channel->log` unchanged.

**Publishing**

- Client channels become generic publishers.
- The bytes are CDR and are forwarded as they are. MITI encodes them with the schema of the
  topic's channel, or from `miti.getSchema`. The bridge needs no conversion code.

**Services**

- The bridge offers the services found in the graph, with schemas split from the `.srv`
  definition.
- Humble's rclcpp has no generic service client, so this is written on rcl, loading the type
  support by name, for both distros. `prototypes/generic_service_client` shows how, and works on
  both.

**Parameters:** an async parameter client per node, cached, with a 5 s timeout.

**Assets:** `libmiti`'s roots are set to the share directories of the packages in the ament index,
and `package://` URIs are resolved against them.

**Time:** forwards `/clock`.

**Configuration:** ROS parameters, plus the environment variables MITI's launcher already sets for
foxglove_bridge (`BRIDGE_PORT`, `BRIDGE_ADDRESS`, `BRIDGE_TOPIC_WHITELIST`, `ROS_DOMAIN_ID`,
`ROS_STATIC_PEERS`, …), so `/api/bridge` and `.miti/bridge.json` keep working.

## Repository: `MITI-sdk`

MITI has two SDKs, in two repositories:

| Repository        | Side    | Holds                                                     |
| ----------------- | ------- | --------------------------------------------------------- |
| `MITI-sdk`        | Robot   | libmiti and miti_bridge (this design)                     |
| `MITI-widget-sdk` | Browser | `@miti/sdk`, the types and build tools for MITI's widgets |

The widget SDK was in `MITI-sdk` until 2026-11-02. It moved when this repository took the robot
side.

```
MITI-sdk/
├── core/             libmiti: include/miti/, src/, tests/ (gtest); a plain CMake package with a package.xml for colcon
├── python/           nanobind binding; pyproject.toml (scikit-build-core); tests/ (pytest)
├── ros/miti_bridge/  the ROS 2 package
├── schemas/          vendored .msg files, and the generator for typed messages
├── conformance/      protocol conformance suite (TypeScript), run against any server
├── examples/         C++ and Python: camera, TF, service, Isaac Sim
└── docs/             this design, protocol.md, and later the guides and API reference
```

**Versions**

- Tags are `vX.Y.Z`. libmiti, the Python package and miti_bridge are released together, with
  one version.
- 0.x during the phases below. 1.0 once miti_bridge runs on the robots and the Python API has
  been used in a real project, the Isaac Sim example.
- The protocol version (`miti.bridge.v1`) is separate from the release version.

**Distribution**

| What        | How                                                                                                                                                                                          |
| ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| miti_bridge | `ros-humble-miti-bridge` and `ros-jazzy-miti-bridge` .debs, built in CI with bloom, in MITI's apt repository (`hiran-t.github.io/MITI`); Docker image `ghcr.io/hiran-t/miti-bridge:<distro>` |
| C++         | A `libmiti` .deb with CMake config in the same apt repository; source builds with CMake                                                                                                      |
| Python      | Wheels built with cibuildwheel: manylinux x86_64/aarch64, macOS arm64, Windows x64                                                                                                           |

## Changes in MITI

While both bridges are supported:

- **Own protocol layer.** Replace `FoxgloveClient` from `@foxglove/ws-protocol` with
  `src/lib/bridge/protocol.ts`, which encodes and parses the base and the `miti.` ops.
  `FoxgloveClient` cannot send extension operations. Keep `@foxglove/rosmsg`,
  `rosmsg2-serialization` and `cdr`.
- **Publishing.** Over `miti.bridge.v1`, `publish()` encodes CDR instead of JSON. The schema comes
  from the topic's channel, or from `miti.getSchema`.
- **Per-subscription decoders.** A lane switches a subscription's decoder when
  `miti.subscriptionFormat` arrives.
- **Rate limits.** `useTopic`'s `maxRate` and `throttleMs` go to `miti.subscriptionOptions`.
- **Panel settings.**
  - Image panel: Compression _Auto_, _Off_, or a quality.
  - 3D Scene point cloud layers: Reduce _Off_, a voxel size, or a maximum number of points.
- **Connection status.** Shows which bridge is connected, from `serverInfo.name` and the version,
  and link health from `miti.stats`.
- **Launcher and packaging.** miti_bridge becomes the bridge MITI starts:
  - `ros/miti-bridge.sh`
  - `scripts/start-miti.mjs` and `is_bridge` recognise `miti.bridge.v1`
  - `pixi.toml` depends on miti_bridge
  - `ros/Dockerfile` and `packaging/miti-bridge.service` use it
  - `MITI_BRIDGE=foxglove` keeps foxglove_bridge for robots that have not switched yet
- **Docs.** The how-to guides "Run foxglove_bridge beside MITI" and "on the robot" become
  miti_bridge guides.
- **Widget SDK.** API v1 does not change. `hasCapability` can report `miti.transforms`.

When the robots run miti_bridge, a major MITI release removes the rest of foxglove_bridge:

- The Foxglove subprotocols, `separate` stream mode and the lanes for heavy topics
- `MITI_BRIDGE=foxglove`
- `ros/foxglove-bridge.sh` and `ros/foxglove-bridge.ps1`
- `@foxglove/ws-protocol`
- `scripts/heavy-bridge.js`, replaced by a `libmiti` example server

## Testing

- **Core:** gtest, with no ROS. It covers protocol encoding, the scheduler (priority, newest
  wins, backpressure on a slow socket), transforms, CDR round-trips of every typed message, and
  asset path escapes.
- **Python:** pytest against the wheel. It covers numpy without copies, the GIL released, and
  services.
- **Conformance** (TypeScript)
  - Checks every row of the base table and every extension.
  - Runs against a `libmiti` example server, and against miti_bridge on a fixture ROS graph.
  - Also runs the base checks against foxglove_bridge. MITI talks to both until the switch, so the
    base has to behave the same on each.
- **CI**
  - Core and Python on Linux, macOS and Windows.
  - miti_bridge in `ros:humble` and `ros:jazzy`, on amd64 and arm64 runners, and on Windows with
    RoboStack.
- **Load**
  - Three 1280×720 rgb8 cameras at 15 Hz, a 100k-point cloud at 10 Hz and TF at 100 Hz, as in
    `scripts/heavy-bridge.js`. Measured with `scripts/perf-run.js`.
  - Over a link limited with `tc netem` (20 Mbit/s, 30 ms), against foxglove_bridge.
- **Hardware:** the Isaac Sim G1 machine (Jazzy) and the pacen robot (Humble), confirmed on site.

Targets, under that load on the 20 Mbit/s link:

- TF from stamp to browser: p95 under 150 ms while cameras stream on the same connection.
- One 1280×720 raw camera with JPEG quality 80: at least 10 fps, under 300 ms behind. Today it is
  1 fps and 2.6 s behind.
- miti_bridge CPU with no transforms: no more than foxglove_bridge's. With transforms, to be
  measured on a Jetson Orin.

## Phases

MITI keeps working with foxglove_bridge until phase 6.

| Phase                      | Delivers                                                                                                                                                                                | Done when                                                                                       | Rough size |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- | ---------- |
| 0. Spec & harness          | protocol.md; conformance suite; fixture graph; repository skeleton with CI; prototype of the generic service client on Humble                                                           | The base checks pass against foxglove_bridge on Humble and Jazzy; the prototype calls a service | ~1 week    |
| 1. Core                    | Server, sessions and scheduler, raw channels, callbacks, assets, C++ API, gtest                                                                                                         | The conformance suite passes against a C++ example server                                       | 3 weeks    |
| 2. miti_bridge             | Graph, definitions, topics, publishing, services, parameters, assets, time                                                                                                              | MITI shows every panel on the fixture and the G1 sim; URDF from a parameter and a package       | 2–3 weeks  |
| 3. Transforms & MITI       | JPEG, reduction, stats; MITI's protocol layer, CDR publishing and settings                                                                                                              | The link targets are met over netem                                                             | 2 weeks    |
| 4. Typed messages & Python | Generated message types; nanobind binding; wheels; examples, including Isaac Sim                                                                                                        | The Isaac Sim example streams a camera and TF to MITI without ROS                               | 2–3 weeks  |
| 5. Ship                    | .debs, wheels, Docker image; MITI starts miti_bridge by default; guides                                                                                                                 | Install from apt on a clean machine; the robot runs miti_bridge from apt                        | 2 weeks    |
| 6. Drop foxglove_bridge    | A major MITI release without the Foxglove path                                                                                                                                          | No robot in use needs foxglove_bridge                                                           | ~1 week    |
| Later                      | Token authentication; MCAP recording; H.264 through the Jetson hardware encoder; depth as PNG; changing ROS_DOMAIN_ID without a restart; a C ABI for other languages; a multi-robot hub |                                                                                                 |            |

In total, about 13–15 weeks. Sizes are rough. They assume one developer working with Claude, with
the robot available for testing. Phases 3 and 4 are independent once phase 1 is done.

**Progress (2026-11-02): phase 0 is done.**

- **Protocol:** protocol.md.
- **Conformance suite and fixture graph:** `conformance/`. foxglove_bridge 3.6 passes every base
  check on Humble and Jazzy.
- **Service-client prototype:** `prototypes/generic_service_client`, which works on both distros.
- **CI:** `.github/workflows/ci.yml`. It runs both of the above in `ros:<distro>-ros-base`:
  Humble on Ubuntu 22.04 and Jazzy on 24.04, each on amd64 and arm64.
- **Development environments:** pixi, `pixi.toml`.

**Progress: phase 1 is done.** `core/` holds libmiti, which passes the conformance suite against
its example server.

- **What it implements:**
  - the base protocol
  - `miti.subscriptionOptions` (`maxRate`, `delivery`, `priority`) and the scheduler
  - assets with an allowlist
  - the C++ API, installable with `find_package(libmiti)`
- **How it is checked:** 30 gtests, and clean runs under AddressSanitizer, UndefinedBehaviorSanitizer
  and ThreadSanitizer.
- **What it builds on:** from Ubuntu 22.04 (GCC 11, Boost 1.74) to macOS (Clang, Boost 1.92). CI
  checks Ubuntu 22.04 and 24.04, each on amd64 and arm64.

Found along the way, for phase 2:

- **Latched topics.** `/tf_static` and `/robot_description` are published once, as transient-local.
  A second client that subscribes after the first never sees them, because the bridge's ROS
  subscription already took the message. libmiti needs latched channels, which keep their last
  messages for new subscribers. Done in phase 2.
- **Asset reads.** Assets are read on the server's thread. A mesh of a few megabytes is fast to
  read, but a slow disk would stall every connection meanwhile. Reading on another thread is
  phase 2's work. Done in phase 2.

**Progress: phase 2 is done.** `ros/miti_bridge` is the ROS 2 node, and passes the conformance suite
on Humble and Jazzy (25 checks). It covers:

- graph discovery
- definitions from the ament index
- topics with QoS that matches the publishers
- latched transient-local topics
- client publishing
- services through an event-driven generic client (an `rclcpp::ClientBase`, the same on both
  distros)
- parameters, package assets and the clock

The suite gained a check that a transient-local topic reaches a client that subscribes later.
foxglove_bridge passes it too.

Not done:

- **MITI against miti_bridge.** Done in phase 3a.
- **The G1 simulation.** It was not reachable on the office network when this phase was done.

**Progress: phase 3a is done. MITI works with miti_bridge** (MITI branch `feat/miti-bridge`).

- **In MITI:**
  - **Protocol client.** `src/lib/bridge/protocol.ts` replaces `FoxgloveClient` and keeps its API.
    It offers `miti.bridge.v1` first, and sends `miti.*` operations only to a server that selected
    it.
  - **Publishing.** Over miti_bridge it is CDR. A type that no topic advertises is looked up once
    with `miti.getSchema`.
  - **Rate limits.** Each topic's limits go to the bridge as `miti.subscriptionOptions`, and the
    client then skips its own `maxRate` gate.
  - **Status bar.** It names the bridge MITI is connected to.
- **In libmiti and miti_bridge:**
  - **`miti.getSchema`:** libmiti's `get_schema` callback, and the bridge's definitions from the
    ament index.
  - **`maxRate`:** it tolerates 10 %, so a source at exactly `maxRate` is not halved by jitter.
- **Checked in a browser.** MITI showed the fixture graph's topics and messages from miti_bridge.
  - Its Button Switch published a `std_msgs/String`, which the fixture echoed back.
  - It published a `std_msgs/Int32` on a topic that nothing advertised; ROS received `data: 42`.
  - The same MITI still worked with foxglove_bridge.

**Progress: phase 3b is done. Transforms, formats and link stats work end to end.**

- **In libmiti:**
  - **Transforms.** `jpeg` (TurboJPEG) and `reduce`, run on two threads of their own.
    Subscribers that ask for the same transform of a channel share one run.
  - **`miti.subscriptionFormat`** before the first message in a new format.
  - **`miti.stats`.** The server reports each connection's subscriptions: bytes per second, sent,
    dropped, and how long the oldest waiting message has waited.
  - **Sockets.** `TCP_NODELAY`, and at most 128 KB unsent in the kernel (`TCP_NOTSENT_LOWAT`), so
    frames wait where a newer one can replace them.
- **In MITI:**
  - **Subscriptions** take a `transform`. It is sent only when every subscriber of the topic asks
    for the same one, since they all receive its output.
  - **Image panel.** It has Compression: JPEG on robot (the default) or Off. The 3D Scene's camera
    images ask for the same. `jpeg` waits for a raw frame in an encoding JPEG can hold, so depth
    images stay raw.
  - **Point clouds.** The 3D Scene asks for `reduce` to its Point limit, so points over the limit
    never cross the network.
  - **Status bar and source details** show the link's bytes per second, drops and backlog.
- **Checked:** 50 gtests, clean under the sanitizers, and on Ubuntu 22.04 in Docker (Boost 1.74,
  TurboJPEG 2.1). Conformance has 33 checks, and libmiti and miti_bridge pass them on Humble and
  Jazzy. The Humble .deb passes them too, installed in a fresh container; it brings `libturbojpeg`.

**The link targets, measured** with `bench/link/run.sh`. The setup:

- A robot publishes a raw 1280×720 rgb8 camera at 30 Hz and TF at 50 Hz.
- The bridge is behind `tc netem` at 20 Mbit/s with 30 ms of delay.
- The client asks for the camera at 15 fps, as MITI's Image panel does by default.

| Bridge, camera        | TF late, p50 / p95 | Camera              | Camera late, p95 |
| --------------------- | ------------------ | ------------------- | ---------------- |
| miti_bridge, JPEG q80 | 35 / 62 ms         | 14.3 fps, 86 KB     | 88 ms            |
| miti_bridge, raw      | 1.4 / 1.9 s        | 0.9 fps, 2.7 MB     | 2.2 s            |
| foxglove_bridge, raw  | 11 / 14 s          | 0.9 fps (every one) | 15 s             |

Both targets are met: TF p95 is under 150 ms with the camera streaming, and the camera arrives at
10 fps or more, less than 300 ms behind.

- **foxglove_bridge.** It queues every frame, so TF arrived once a second, 14 s late.
- **Raw frames.** A raw frame takes 1.1 s to cross this link, and TF waits behind the frame being
  sent.
- **Overload.** A camera at every frame (28 fps of JPEG) asks for more than the link carries. TF
  then arrives about 1 s late: TCP keeps the link's buffer full. Keeping under the link's capacity
  is what keeps it fast. MITI's default of 15 fps does that here; adapting the rate to the link is
  later work.
- **Unshaped,** the raw camera arrives at 29.7 fps (83 MB/s), and TF's p95 is 7 ms. The socket
  limit costs no throughput.

**Progress: Windows (issue #1). libmiti and miti_bridge build and run on Windows x64.**

- **How it builds:** from source with pixi. MSVC comes through conda-forge's `vs2022_win-64`,
  which uses the Visual Studio 2022 C++ tools on the machine; Boost, nlohmann_json and TurboJPEG
  from conda-forge; Humble and Jazzy from RoboStack, which has every package the bridge, the
  fixture graph and foxglove_bridge need. There is no package for Windows.
- **What Windows needed:**
  - **Warnings.** `/W4` with warnings as errors, as `-Wall -Wextra -Werror` elsewhere. A shared
    libmiti exports every symbol, as it does with GCC and Clang.
  - **Sockets.** `TCP_NODELAY` as elsewhere. Windows has no `TCP_NOTSENT_LOWAT`; its send buffer
    follows the connection's ideal send backlog. `SO_REUSEADDR` is set only off Windows: there
    it would let a second server bind a port in use, and a stopped server's port is free again
    without it.
  - **Assets.** `file:///C:/…` names a file on a drive. A path whose root is not a drive
    (`\\host\share`) is refused before any lookup, which would reach out to that host. A file's
    8.3 short name does not make it an asset.
  - **Conformance suite.** It runs from Git Bash.
- **Checked in CI on Windows:** 56 gtests, a shared build, and the conformance suite against the
  example server (33). On Humble and Jazzy: colcon tests, the launch file, and the suite against
  miti_bridge (33). foxglove_bridge from RoboStack passed the base checks in four of five runs; in
  the other it did not see the fixture's topics within 30 s, so CI runs it on Linux only.
- **Not checked:** a Windows PC other than GitHub's runners, and a slow link: `bench/link` needs
  `tc netem`, which is Linux only.
- **In MITI, after a release:** the bridge beside MITI on Windows becomes miti_bridge, built from
  source like on Linux and macOS, instead of foxglove_bridge.

Phase 4 is next: typed messages and the Python binding.

## Risks

- **Generic service calls on Humble.** Settled in phase 0. `prototypes/generic_service_client`
  calls a service of a type known only at run time, with CDR in and out, on Humble and Jazzy. It
  uses rcl, type support looked up by symbol name, and `rmw_(de)serialize`.
- **A public API is a long-term promise.** Breaking changes are allowed during 0.x and need a
  major version after 1.0. Keep the 1.0 surface small: typed channels, raw channels, services,
  parameters, assets and time.
- **Python wheels across three operating systems:** the most build work in the plan. cibuildwheel
  and the GIL rules need tests from phase 4 on, not at release.
- **Asset serving:** path normalization and the allowlist need tests, not just review.
- **Robot CPU:** transforms spend robot CPU. They are opt-in per subscription.
- **Mixed graphs:** `.idl`-only types, and graphs that mix ROS distros. The bridge's distro must
  match the robot's.
- **Maintenance:** each new ROS 2 distro means a CI matrix entry and a release. Each new Python
  version means another set of wheels.
- **The switch:** a robot in the field still on foxglove_bridge after phase 6 needs an older MITI.
  Phase 6 waits until none is left.

## Decided

| Date       | Decision                                                                                                                                                                                                                                                                                                                                                                                                   |
| ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --- |
| 2026-11-02 | Public, MIT. A library that robot programs build on spreads only if anyone can use it. If only MITI-Pro had the bridge, free MITI would depend on foxglove_bridge forever. Pro features, such as a multi-robot hub, sign-in or fleet views, can build on top of it.                                                                                                                                        |
| 2026-11-02 | miti_bridge replaces foxglove_bridge in MITI, and MITI drops foxglove_bridge once robots have switched. miti_bridge serves MITI only, so it publishes CDR only and does not offer Foxglove's subprotocols.                                                                                                                                                                                                 |
| 2026-11-02 | It lives in `MITI-sdk`, the robot side's repository. The widget SDK moved out to `MITI-widget-sdk`.                                                                                                                                                                                                                                                                                                        |
| 2026-11-02 | Installed with `sudo apt install ros-$ROS_DISTRO-miti-bridge` and started with `ros2 launch miti_bridge miti_start`. The .debs come from MITI-sdk's own signed apt repository first, one suite per Ubuntu release. They go to the ROS buildfarm once the API is stable (1.0). This part of phase 5 came early: .debs built with bloom, and checked on every change by installing them in fresh containers. |

## Open decisions

1. **Names**
   - C++ library: `libmiti`, namespace `miti`
   - Python: distribution name to be checked on PyPI, `import miti`
   - ROS package: `miti_bridge`
   - Subprotocol: `miti.bridge.v1`
