# miti_bridge

A ROS 2 node that serves the robot's topics, services, parameters and package files to MITI over [the MITI protocol](../../docs/protocol.md). It is built on [libmiti](../../core/README.md), and replaces foxglove_bridge for MITI. Supported: Humble and Jazzy.

## Run

Build it with libmiti in a workspace, then run it:

```bash
colcon build --merge-install --base-paths core ros
```

```bash
install/lib/miti_bridge/miti_bridge --ros-args -p port:=8765
```

With pixi, from the repository root:

```bash
pixi run -e humble build-bridge         # into install/humble (-e jazzy: install/jazzy)
pixi run -e humble test-bridge          # the tests of libmiti and miti_bridge
pixi run -e humble conformance-bridge   # the conformance suite against it, with the fixture graph
```

## Parameters

| Parameter                | Default                                                 | What it does                                                                           |
| ------------------------ | ------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `port`                   | `8765`                                                  | WebSocket port                                                                         |
| `address`                | `0.0.0.0`                                               | Address to listen on                                                                   |
| `topic_whitelist`        | `['.*']`                                                | Regular expressions of the topics to offer                                             |
| `service_whitelist`      | `['.*']`                                                | The services to offer                                                                  |
| `param_whitelist`        | `['.*']`                                                | The parameters clients may read, as `/node.parameter`                                  |
| `client_topic_whitelist` | `['.*']`                                                | The topics clients may publish on                                                      |
| `capabilities`           | `['clientPublish', 'parameters', 'services', 'assets']` | What clients may do                                                                    |
| `max_qos_depth`          | `10`                                                    | The largest queue a subscription keeps                                                 |
| `asset_roots`            | `[]`                                                    | Folders `file://` assets may come from. `package://` works for every installed package |
| `use_sim_time`           | `false`                                                 | Send the robot's `/clock` to clients, and stamp messages with it                       |

The parameter names are the ones foxglove_bridge uses, so MITI's launch scripts can pass the same settings.

## How it works

- **Graph.** Every second the node reads the topic and service lists. It offers each topic whose `.msg` definition is installed. Hidden names (with a part that starts with `_`) and its own services are left out. A type it cannot offer, for example one that ships only as `.idl`, or a package that is not installed, is reported once.
- **Topics.**
  - A ROS subscription exists only while a client is subscribed.
  - Its QoS matches the publishers: reliable unless one is best effort, transient-local if all are, and deep enough for their history.
  - The serialized bytes go to clients unchanged.
- **Transient-local topics.** `/tf_static` and `/robot_description` are latched: the newest message of each transient-local publisher is kept for clients that subscribe later.
- **Client publishing.** A client channel becomes a generic publisher, with CDR forwarded as it is.
- **Services.**
  - Calls go through a generic client: an `rclcpp::ClientBase` initialized with type support looked up by name, with requests and responses deserialized and serialized through the introspection type support. Humble has no `GenericClient`, so this one is used on both distros. Responses arrive through the executor.
  - A call fails after 10 s without a response.
- **Parameters.** `/node.parameter` is read with that node's parameter service. A node that does not answer within 5 s counts as having none.
- **Assets.** `package://` resolves against every package in the ament index.

## Not yet

- MITI connects only once it offers `miti.bridge.v1`, and it publishes JSON while miti_bridge takes CDR. Both are phase 3 of [the design](../../docs/design.md).
- Each message is copied once on its way from ROS to the clients.
- One executor thread serves subscriptions, service responses and the graph.
