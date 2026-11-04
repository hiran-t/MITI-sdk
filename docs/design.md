# MITI bridge: design

Status: draft, 2026-11-04.

**Purpose.** miti_bridge is MITI's bridge on the robot. It is built on a library of our own,
`libmiti`, which any program can use to stream to MITI, with or without ROS.

**Where it lives.** In the `MITI-sdk` repository, which holds the robot side of MITI. It is
public, under the MIT license. The widget SDK, for the browser side, is in `MITI-widget-sdk`.

**Related.** The remaining questions are in [Open decisions](#open-decisions), and the wire format
is in [protocol.md](protocol.md). Paths such as `src/lib/bridge/client.ts` or
`scripts/heavy-bridge.js` refer to the [MITI](https://github.com/hiran-t/MITI) repository.

## Why

**Ownership.** The program that runs on the robot decides what MITI can do: its protocol, its
features and how fast it changes. MITI needs that program to be its own.

**The link.** Without a way to ask the robot for less data, MITI had to work around it in the
browser (`src/lib/bridge/client.ts`):

- `maxRate` and `throttleMs` dropped messages after they had already crossed the network.
- Heavy topics needed WebSockets of their own so TF was not queued behind camera frames. Panels
  then drifted apart in time.
- A raw 1280×720 rgb8 frame is 2.8 MB. On the robot's link that measured 1 fps, 2.6 s behind.
- Images and point clouds were decoded in full in Web Workers, even when a panel drew only part
  of them.

Only the sending side can fix this, and it has to run **on the robot**. A bridge beside MITI
subscribes over DDS, so raw data still crosses the link before the bridge can reduce it.

**Why a library under the bridge.**

- Programs without ROS can stream to MITI directly. Examples: Isaac Sim scripts, PLC and fieldbus
  gateways, vision applications, test rigs.
- The core is tested without DDS.
- Other middleware (ROS 1, Zenoh, MQTT) can be added later as adapters.

## Goals

1. **miti_bridge is MITI's bridge.**
   - It is the bridge MITI installs, starts and documents.
   - MITI's panels and `@miti/sdk` widgets work with it unchanged, including
     `MitiClient.hasCapability`.
2. **Fix the link.** On the robot, before data is sent: per-subscription rate limits, priority,
   newest-wins delivery for heavy topics, JPEG for raw images, point cloud reduction.
3. **`libmiti`, a library that serves MITI over WebSocket.**
   - miti_bridge is built on it, and so can any other program.
   - Programs use it to publish topics, provide services, parameters and assets, and receive what
     MITI publishes.
   - It is to include typed messages for everything MITI's panels draw, so no ROS installation is
     needed, and a Python binding.
   - The link fixes live in its core, so every program built on it gets them.
4. **Platforms.**
   - `libmiti`: Linux (amd64, arm64 including Jetson), macOS and Windows.
   - miti_bridge: Ubuntu 22.04/24.04 with Humble and Jazzy.

Not in the first release:

- ROS 1
- MCAP recording and playback
- A C ABI and bindings for other languages (C#, Rust)
- `setParameters`, parameter subscriptions and the connection graph. MITI does not use them.
- Authentication (see [Later](#later)).
- Clients other than MITI.

## Approach: MITI's own protocol

The MITI protocol (`miti.bridge.v1`) has two parts:

- **Base.** Topics, services, parameters, assets and time: the operations every MITI panel uses.
- **Extensions.** MITI's flow control and transforms on top of the base.

miti_bridge offers only the subprotocol `miti.bridge.v1`. `libmiti` is written from protocol.md.

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
     - The exception is compressed video, which stays `all`: dropping a video frame breaks the
       frames that depend on it.
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
- Latched channels keep their last messages for clients that subscribe later. `/tf_static` and
  `/robot_description` are published once, as transient-local, so a second client would otherwise
  never see them.
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

The session writes one message at a time, with `TCP_NODELAY`, and keeps at most 128 KB unsent in
the kernel (`TCP_NOTSENT_LOWAT`). Messages wait in the scheduler instead, where a newer frame can
replace an older one and TF goes first, so one connection keeps TF real-time. Asset downloads
still use their own connection, because one asset is one WebSocket message of several megabytes.

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
  symlinks are followed: `colcon build --symlink-install` installs every file of a package as a
  link into the source tree, so refusing them broke every such workspace (found on the Isaac Sim
  G1 in v0.1.0).
- Assets are read off the server's thread, so a slow disk does not stall the connections.
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

The binding is to use nanobind. It releases the GIL while logging and while I/O runs, and passes
numpy arrays through the buffer protocol, without copies.

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
  `/robot_description`, which go to latched channels.
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

**Configuration:** ROS parameters, plus the environment variables MITI's launcher sets for the
bridge beside MITI (`BRIDGE_PORT`, `BRIDGE_ADDRESS`, `BRIDGE_TOPIC_WHITELIST`, `ROS_DOMAIN_ID`,
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

`python/`, `schemas/` and `examples/` are planned.

**Versions**

- Tags are `vX.Y.Z`. libmiti, the Python package and miti_bridge are released together, with
  one version.
- 0.x until the API is stable. 1.0 once miti_bridge runs on the robots and the Python API has
  been used in a real project, the Isaac Sim example.
- The protocol version (`miti.bridge.v1`) is separate from the release version.

**Distribution**

| What        | How                                                                                                                                                    |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| miti_bridge | `ros-humble-miti-bridge` and `ros-jazzy-miti-bridge` .debs, built in CI with bloom, in MITI-sdk's signed apt repository (`hiran-t.github.io/MITI-sdk`) |
| C++         | A `libmiti` .deb with CMake config in the same apt repository; source builds with CMake                                                                |
| Python      | Wheels built with cibuildwheel: manylinux x86_64/aarch64, macOS arm64, Windows x64                                                                     |

## MITI's side

- **Protocol layer.** `src/lib/bridge/protocol.ts` encodes and parses the base and the `miti.` ops.
- **Publishing.** `publish()` encodes CDR. The schema comes from the topic's channel, or from
  `miti.getSchema`.
- **Per-subscription decoders.** A subscription switches its decoder when
  `miti.subscriptionFormat` arrives.
- **Rate limits.** `useTopic`'s `maxRate` and `throttleMs` go to `miti.subscriptionOptions`.
- **Panel settings.**
  - Image panel: Compression, JPEG on the robot (the default) or Off.
  - 3D Scene point cloud layers: the Point limit, applied with `reduce`.
- **Connection status.** Shows which bridge is connected, from `serverInfo.name` and the version,
  and link health from `miti.stats`.
- **Launcher and packaging.** miti_bridge is the bridge MITI starts beside it: `ros/miti-bridge.sh`,
  `scripts/start-miti.mjs`, `pixi.toml`, `ros/Dockerfile` and `packaging/miti-bridge.service`.
- **Widget SDK.** API v1 does not change. `hasCapability` can report `miti.transforms`.

## Testing

- **Core:** gtest, with no ROS. It covers protocol encoding, the scheduler (priority, newest
  wins, backpressure on a slow socket), transforms, CDR round-trips, and asset path escapes.
  Clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.
- **Python:** pytest against the wheel, once the binding exists. It covers numpy without copies,
  the GIL released, and services.
- **Conformance** (TypeScript)
  - Checks every row of the base table and every extension.
  - Runs against a `libmiti` example server, and against miti_bridge on a fixture ROS graph.
- **CI**
  - libmiti on Ubuntu 22.04 and 24.04, each on amd64 and arm64, plus a sanitizer build.
  - miti_bridge in `ros:humble` and `ros:jazzy`, on amd64 and arm64 runners, and the .debs
    installed in fresh containers.
- **Load**
  - Three 1280×720 rgb8 cameras at 15 Hz, a 100k-point cloud at 10 Hz and TF at 100 Hz, as in
    `scripts/heavy-bridge.js`. Measured with `scripts/perf-run.js`.
  - Over a link limited with `tc netem` (20 Mbit/s, 30 ms): `bench/link/run.sh`.
- **Hardware:** the Isaac Sim G1 machine (Jazzy) and the pacen robot (Humble), confirmed on site.

Targets, on the 20 Mbit/s link:

- TF from stamp to browser: p95 under 150 ms while cameras stream on the same connection.
- One 1280×720 raw camera with JPEG quality 80: at least 10 fps, under 300 ms behind.
- miti_bridge's CPU use with transforms, to be measured on a Jetson Orin.

### The link, measured

With `bench/link/run.sh`. The setup:

- A robot publishes a raw 1280×720 rgb8 camera at 30 Hz and TF at 50 Hz.
- The bridge is behind `tc netem` at 20 Mbit/s with 30 ms of delay.
- The client asks for the camera at 15 fps, as MITI's Image panel does by default.

| Camera                | TF late, p50 / p95 | Camera          | Camera late, p95 |
| --------------------- | ------------------ | --------------- | ---------------- |
| JPEG q80 on the robot | 35 / 62 ms         | 14.3 fps, 86 KB | 88 ms            |
| Raw                   | 1.4 / 1.9 s        | 0.9 fps, 2.7 MB | 2.2 s            |

Both targets are met: TF p95 is under 150 ms with the camera streaming, and the camera arrives at
10 fps or more, less than 300 ms behind.

- **Raw frames.** A raw frame takes 1.1 s to cross this link, and TF waits behind the frame being
  sent.
- **Overload.** A camera at every frame (28 fps of JPEG) asks for more than the link carries. TF
  then arrives about 1 s late: TCP keeps the link's buffer full. Keeping under the link's capacity
  is what keeps it fast. MITI's default of 15 fps does that here; adapting the rate to the link is
  later work.
- **Unshaped,** the raw camera arrives at 29.7 fps (83 MB/s), and TF's p95 is 7 ms. The socket
  limit costs no throughput.

## Later

- Token authentication
- MCAP recording
- H.264 through the Jetson hardware encoder
- Depth images as PNG
- Changing `ROS_DOMAIN_ID` without a restart
- A C ABI for other languages
- A multi-robot hub

## Risks

- **Generic service calls on Humble.** Settled: `prototypes/generic_service_client` calls a service
  of a type known only at run time, with CDR in and out, on Humble and Jazzy. It uses rcl, type
  support looked up by symbol name, and `rmw_(de)serialize`.
- **A public API is a long-term promise.** Breaking changes are allowed during 0.x and need a
  major version after 1.0. Keep the 1.0 surface small: typed channels, raw channels, services,
  parameters, assets and time.
- **Python wheels across three operating systems:** the most build work in the plan. cibuildwheel
  and the GIL rules need tests from the binding's first commit, not at release.
- **Asset serving:** path normalization and the allowlist need tests, not just review.
- **Robot CPU:** transforms spend robot CPU. They are opt-in per subscription.
- **Mixed graphs:** `.idl`-only types, and graphs that mix ROS distros. The bridge's distro must
  match the robot's.
- **Maintenance:** each new ROS 2 distro means a CI matrix entry and a release. Each new Python
  version means another set of wheels.

## Decided

| Date       | Decision                                                                                                                                                                                                                                                                                                                                                           |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 2026-11-02 | Public, MIT. A library that robot programs build on spreads only if anyone can use it. If only MITI-Pro had the bridge, free MITI would have no bridge of its own. Pro features, such as a multi-robot hub, sign-in or fleet views, can build on top of it.                                                                                                        |
| 2026-11-02 | miti_bridge is MITI's bridge. It serves MITI only, so it publishes CDR only and offers only `miti.bridge.v1`.                                                                                                                                                                                                                                                      |
| 2026-11-02 | It lives in `MITI-sdk`, the robot side's repository. The widget SDK moved out to `MITI-widget-sdk`.                                                                                                                                                                                                                                                                |
| 2026-11-02 | Installed with `sudo apt install ros-$ROS_DISTRO-miti-bridge` and started with `ros2 launch miti_bridge miti_start`. The .debs come from MITI-sdk's own signed apt repository first, one suite per Ubuntu release, built with bloom and checked on every change by installing them in fresh containers. They go to the ROS buildfarm once the API is stable (1.0). |

## Open decisions

1. **Names**
   - C++ library: `libmiti`, namespace `miti`
   - Python: distribution name to be checked on PyPI, `import miti`
   - ROS package: `miti_bridge`
   - Subprotocol: `miti.bridge.v1`
