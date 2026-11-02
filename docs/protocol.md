# MITI protocol, version 1

Status: draft, 2026-11-02.

This document describes how MITI talks to a robot over WebSocket, with the subprotocol
`miti.bridge.v1`. It has two parts:

- **The base protocol** carries topics, services, parameters, assets and time. Its JSON and binary
  framing is the same as the part of the Foxglove WebSocket protocol v1 that MITI uses. That way,
  while some robots still run foxglove_bridge, MITI reads both kinds of robot through one code
  path.
- **The extensions** are operations of MITI's own, for flow control and transforms.

`libmiti`, and so miti_bridge, implement both. The conformance suite in `conformance/`
checks a server against this document.

The key words MUST, SHOULD and MAY are used as in RFC 2119.

## Conventions

- **Text frames** carry one JSON object each, with an `op` field naming the operation.
- **Binary frames** start with a one-byte opcode. Every integer in them is little-endian.
- **Strings** inside binary frames are UTF-8, prefixed with their byte length as a `uint32`.
- **Timestamps** are nanoseconds as `uint64`. They are Unix time, or the robot's clock when it
  runs on simulation time.
- **Ids** (channels, subscriptions, services, client channels, calls, asset requests) are
  `uint32`. Within one connection, an id is never reused for something else.
- **Unknown fields and operations.** Receivers ignore unknown fields. A server that receives an
  unknown operation answers with a `status` warning and keeps the connection open.

## Connection

A server conforming to this document accepts the subprotocol `miti.bridge.v1`, and no other. It
refuses a handshake that does not offer it.

Until MITI drops foxglove_bridge, MITI offers `miti.bridge.v1`, `foxglove.sdk.v1` and
`foxglove.websocket.v1`, in that order. When foxglove_bridge selects one of its own subprotocols,
MITI uses only the base protocol and sends no extension operations. Its JSON publishing on that
path is described in foxglove_bridge's documentation, not here.

Right after the handshake, the server sends, in this order:

1. `serverInfo`
2. `advertise` for its current channels, if there are any
3. `advertiseServices` for its current services, if it has the `services` capability and any
   services

## Base protocol

### Server → client, text

**`serverInfo`**

```json
{
  "op": "serverInfo",
  "name": "miti_bridge",
  "capabilities": ["clientPublish", "services", "parameters", "assets", "time", "miti.transforms"],
  "supportedEncodings": ["cdr"],
  "metadata": { "miti.version": "0.1.0", "ROS_DISTRO": "humble" },
  "sessionId": "1730530000"
}
```

`capabilities` lists what this server allows:

| Capability        | Allows                                             |
| ----------------- | -------------------------------------------------- |
| `clientPublish`   | Client `advertise`, `unadvertise` and message data |
| `services`        | `advertiseServices` and service calls              |
| `parameters`      | `getParameters`                                    |
| `assets`          | `fetchAsset`                                       |
| `time`            | The server sends `time`                            |
| `miti.transforms` | `transform` in `miti.subscriptionOptions`          |
| `miti.stats`      | `miti.subscribeStats`                              |
| `miti.schemas`    | `miti.getSchema`                                   |

Other fields:

- `supportedEncodings`: the message encodings the server accepts from clients that publish.
  Servers conforming to this document list `cdr`.
- `metadata`: free-form strings. `libmiti` sets `miti.version` to its own version.
- `sessionId`: changes when the server restarts. Clients use it to drop state cached from an
  earlier session.

Servers MAY list other capabilities. MITI ignores the ones it does not know.

**`status`** and **`removeStatus`**

- `status` is `{ "op": "status", "level": 0 | 1 | 2, "message": "…", "id"?: "…" }`. The levels
  are info, warning and error.
- `removeStatus` is `{ "op": "removeStatus", "statusIds": ["…"] }`. It withdraws earlier
  `status` messages that had an `id`.

**`advertise`**

```json
{
  "op": "advertise",
  "channels": [
    {
      "id": 3,
      "topic": "/camera/color/image_raw",
      "encoding": "cdr",
      "schemaName": "sensor_msgs/msg/Image",
      "schemaEncoding": "ros2msg",
      "schema": "std_msgs/Header header\nuint32 height\n…"
    }
  ]
}
```

- A topic MAY have several channels, one per message type.
- The encodings MITI decodes:
  - `cdr` with `schemaEncoding` `ros2msg`, which miti_bridge always uses
  - `json`, where `schemaEncoding` may be omitted, for programs on `libmiti` that publish data
    without a ROS type
- A `ros2msg` schema is the type's own definition, followed by each type it uses. Each used type
  comes after a line of 80 `=` and a line `MSG: pkg/Type`.

**`unadvertise`**: `{ "op": "unadvertise", "channelIds": [3] }`. Subscriptions to these channels
end, and no more messages arrive for them.

**`advertiseServices`**

```json
{
  "op": "advertiseServices",
  "services": [
    {
      "id": 1,
      "name": "/reset",
      "type": "std_srvs/srv/Trigger",
      "request": {
        "encoding": "cdr",
        "schemaName": "std_srvs/srv/Trigger_Request",
        "schemaEncoding": "ros2msg",
        "schema": ""
      },
      "response": {
        "encoding": "cdr",
        "schemaName": "std_srvs/srv/Trigger_Response",
        "schemaEncoding": "ros2msg",
        "schema": "bool success\nstring message"
      }
    }
  ]
}
```

Servers MUST send `request` and `response`. While MITI still supports foxglove_bridge, it also
reads the older fields `requestSchema` and `responseSchema`, which foxglove_bridge may send
instead.

**`unadvertiseServices`**: `{ "op": "unadvertiseServices", "serviceIds": [1] }`.

**`parameterValues`**

- Form: `{ "op": "parameterValues", "parameters": [{ "name": "/node.param", "value": …, "type"?: … }], "id"?: "…" }`.
- The server sends it in reply to `getParameters`, with the request's `id`.
- A parameter that does not exist is left out.
- `type` is `byte_array` (the value is base64), `float64` or `float64_array`. It is given only
  where JSON alone is ambiguous.

**`serviceCallFailure`**: `{ "op": "serviceCallFailure", "serviceId": 1, "callId": 7, "message": "…" }`.

### Client → server, text

| Operation       | Fields                                                                             | Needs           |
| --------------- | ---------------------------------------------------------------------------------- | --------------- |
| `subscribe`     | `subscriptions: [{ id, channelId }]`. The client chooses `id`.                     |                 |
| `unsubscribe`   | `subscriptionIds: [id]`                                                            |                 |
| `advertise`     | `channels: [{ id, topic, encoding: "cdr", schemaName, schema?, schemaEncoding? }]` | `clientPublish` |
| `unadvertise`   | `channelIds: [id]`                                                                 | `clientPublish` |
| `getParameters` | `parameterNames: ["/node.param"]`, `id?`                                           | `parameters`    |
| `fetchAsset`    | `uri`, `requestId`                                                                 | `assets`        |

Rules:

- **Unknown channel.** A server answers `subscribe` to a channel it does not have with a `status`
  error and ignores that subscription.
- **Missing capability.** A server answers an operation whose capability it does not list with a
  `status` error.
- **Client channel encoding.** A server answers a client `advertise` whose encoding is not in
  `supportedEncodings` with a `status` error, and does not create that channel.
- **Asset URIs** are `package://pkg/path` or `file:///absolute/path`. Servers resolve them only
  inside their configured roots. A URI outside the roots, an extension that is not allowed, or a
  missing file is a failed `fetchAssetResponse`, never a `status`.

### Binary frames

Server → client:

| Opcode | Name                  | Layout after the opcode                                                                                     |
| ------ | --------------------- | ----------------------------------------------------------------------------------------------------------- |
| `0x01` | message data          | `uint32` subscription id, `uint64` timestamp, message bytes                                                 |
| `0x02` | time                  | `uint64` timestamp                                                                                          |
| `0x03` | service call response | `uint32` service id, `uint32` call id, `uint32` encoding length, encoding, response bytes                   |
| `0x04` | fetch asset response  | `uint32` request id, `uint8` status (0 success, 1 error), `uint32` error length, error message, asset bytes |

Client → server:

| Opcode | Name                 | Layout after the opcode                                                                  |
| ------ | -------------------- | ---------------------------------------------------------------------------------------- |
| `0x01` | client message data  | `uint32` client channel id, message bytes in the client channel's encoding               |
| `0x02` | service call request | `uint32` service id, `uint32` call id, `uint32` encoding length, encoding, request bytes |

Details:

- **Message data timestamp:** the time the server received the message. Panels use the stamp in
  the message's header.
- **Fetch asset errors:** a failed response has an error message and no asset bytes. A successful
  response has an empty error message.
- **Service calls:** the client chooses `callId`. A server answers each call exactly once, with a
  response or a `serviceCallFailure`.

## Extensions (`miti.bridge.v1`)

### `miti.subscriptionOptions` (client → server)

```json
{
  "op": "miti.subscriptionOptions",
  "subscriptionId": 12,
  "maxRate": 10,
  "delivery": "latest",
  "priority": "low",
  "transform": { "kind": "jpeg", "quality": 80, "maxWidth": 960 }
}
```

Fields:

- **`maxRate`:** messages per second, greater than 0. The server sends no more than this. It
  drops messages before they are transformed or queued. `null` or a missing field means no limit.
- **`delivery`:**
  - `all` queues every message, up to a bounded queue; the oldest are dropped when it is full.
  - `latest` keeps one waiting message per subscription, so a newer message replaces it.
- **`priority`:** `high`, `normal` or `low`.
  - The server sends control replies first: JSON operations, service and parameter replies, and
    time.
  - Then `high`, then `normal`, then `low`.
  - Subscriptions of the same priority are served round-robin.
- **`transform`:** see [Transforms](#transforms). `null` removes the transform.

How the client sends it:

- At any time after `subscribe`, including again to change the options.
- Fields that are not given keep their current value.
- Until the first one arrives, the server uses the defaults below.

| Message types                                                                                                                                                                                                                                                                           | Default                                                                   |
| --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `tf2_msgs/msg/TFMessage`, `sensor_msgs/msg/JointState`, `nav_msgs/msg/Odometry`                                                                                                                                                                                                         | `high`, `all`                                                             |
| `sensor_msgs/msg/Image`, `sensor_msgs/msg/CompressedImage`, `sensor_msgs/msg/PointCloud2`, `nav_msgs/msg/OccupancyGrid`, `nav2_msgs/msg/Costmap`, `map_msgs/msg/OccupancyGridUpdate`, `foxglove_msgs/msg/RawImage`, `foxglove_msgs/msg/CompressedImage`, `foxglove_msgs/msg/PointCloud` | `low`, `latest`                                                           |
| `foxglove_msgs/msg/CompressedVideo`                                                                                                                                                                                                                                                     | `low`, `all`. Dropping a video frame breaks the frames that depend on it. |
| Everything else                                                                                                                                                                                                                                                                         | `normal`, `all`                                                           |

**Backpressure.** The server writes to a socket only while the bytes it has buffered for it are
under a limit, by default 256 KB. Messages wait in the queues above, not in the socket.

### `miti.subscriptionFormat` (server → client)

```json
{
  "op": "miti.subscriptionFormat",
  "subscriptionId": 12,
  "encoding": "cdr",
  "schemaName": "sensor_msgs/msg/CompressedImage",
  "schemaEncoding": "ros2msg",
  "schema": "std_msgs/Header header\nstring format\nuint8[] data\n…"
}
```

- **When it is sent:** whenever a transform changes the format of a subscription's messages. That
  includes going back to the channel's own format when a transform is removed.
- **Ordering:** it comes before the first message in the new format. Every later message of that
  subscription uses it. The guarantee holds because text and binary frames share one ordered
  socket.

### Transforms

A transform applies only to its own subscription. Clients that ask for the same transform with the
same parameters on the same channel share one transform run.

When a frame arrives while the previous one is still being transformed, it waits in a single
slot. A newer frame replaces it.

**`jpeg`**

- Parameters: `{ "kind": "jpeg", "quality"?: 1–100 (default 80), "maxWidth"?: pixels }`.
- Input: a `sensor_msgs/msg/Image` with encoding `rgb8`, `bgr8`, `rgba8`, `bgra8` or `mono8`.
- Output: a `sensor_msgs/msg/CompressedImage`.
  - `header` is the input's.
  - `format` is `jpeg`.
  - Alpha is dropped, and `mono8` becomes a grayscale JPEG.
- `maxWidth` scales the image down to that width, keeping its aspect ratio. It never scales up.
- Other encodings, such as `16UC1`, `32FC1` or Bayer, pass through unchanged. The server sends one
  `status` warning per subscription when that happens.

**`reduce`**

- Parameters: `{ "kind": "reduce", "fields"?: ["x", "y", "z", "intensity"], "voxel"?: metres, "stride"?: n, "maxPoints"?: n }`.
- Input: a `sensor_msgs/msg/PointCloud2`.
- Output: a `sensor_msgs/msg/PointCloud2` with `height` 1. The steps run in this order:
  1. **`fields`** keeps only the named fields, repacked without padding. `x`, `y` and `z` are
     always kept.
  2. **`voxel`** keeps the first point in each cube of that size.
  3. **`stride`** keeps every n-th point.
  4. **`maxPoints`** keeps evenly spaced points until no more than n are left.
- Points with a non-finite `x`, `y` or `z` are dropped, and `is_dense` is true.

A transform that the server does not support, or invalid parameters, get a `status` error. The
subscription then continues without a transform.

### `miti.getSchema` and `miti.schema`

- Request: `{ "op": "miti.getSchema", "id": "s1", "type": "std_msgs/msg/Bool" }`.
- Reply: `{ "op": "miti.schema", "id": "s1", "type": "std_msgs/msg/Bool", "schemaEncoding": "ros2msg", "schema": "bool data" }`.
- If the type is not known, the reply has `error` instead of `schemaEncoding` and `schema`.
- Needs the `miti.schemas` capability.

Clients use it to encode CDR for topics no channel advertises yet.

### `miti.subscribeStats`, `miti.unsubscribeStats` and `miti.stats`

- The client sends `{ "op": "miti.subscribeStats", "intervalMs"?: 1000 }` (at least 250).
  `miti.unsubscribeStats` stops the reports.
- The server then reports every interval:

```json
{
  "op": "miti.stats",
  "timestamp": 1730530000000,
  "bufferedBytes": 18432,
  "subscriptions": [
    {
      "subscriptionId": 12,
      "bytesPerSecond": 1520000,
      "sent": 10,
      "dropped": 5,
      "oldestQueuedMs": 40
    }
  ]
}
```

- `sent` and `dropped` count messages since the previous report.
- `dropped` counts every message the server did not send, whether because of `maxRate`, a
  replaced `latest` message, or a full `all` queue.
- `timestamp` is Unix time in milliseconds. It is not nanoseconds, so it stays within JSON's safe
  integer range.
- Needs the `miti.stats` capability.

## Versions

- Within version 1, changes only add optional fields, operations and capabilities.
- Anything else needs a new subprotocol, `miti.bridge.v2`. Servers then accept both for as long as
  MITI releases in use need v1.
