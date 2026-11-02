# miti_bridge

A ROS 2 node that serves the robot's topics, services, parameters and package files to MITI over [the MITI protocol](../../docs/protocol.md). It is built on [libmiti](../../core/README.md), and replaces foxglove_bridge for MITI. Supported: Humble and Jazzy.

## Install

On Ubuntu 22.04 (Humble) or 24.04 (Jazzy), amd64 or arm64, with ROS 2 installed:

```bash
curl -fsSL https://hiran-t.github.io/MITI-sdk/key.gpg | sudo gpg --dearmor -o /usr/share/keyrings/miti-sdk.gpg
```

```bash
echo "deb [signed-by=/usr/share/keyrings/miti-sdk.gpg] https://hiran-t.github.io/MITI-sdk $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | sudo tee /etc/apt/sources.list.d/miti-sdk.list
```

```bash
sudo apt update && sudo apt install ros-$ROS_DISTRO-miti-bridge
```

The repository goes online with the first release (see [docs/releases.md](../../docs/releases.md)). Once the API is stable, the package will also come from ROS's own apt repository, with no repository to add.

## Run

```bash
ros2 launch miti_bridge miti_start
```

Each [parameter](#parameters) is also a launch argument:

```bash
ros2 launch miti_bridge miti_start port:=8765 use_sim_time:=true "topic_whitelist:=['/tf.*', '/camera/.*']"
```

The launch file is installed as `miti_start` and as `miti_start.launch.py`, so both names work. To stop it, press Ctrl+C in its terminal. If another program stops `ros2 launch`, it must stop the whole process group: launch does not pass a TERM signal on to the node.

## Build

Build it with libmiti in a workspace:

```bash
colcon build --merge-install --base-paths core ros
```

```bash
source install/setup.bash && ros2 launch miti_bridge miti_start
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

- MITI connects to miti_bridge from the MITI release after 3.4.2. Earlier versions offer only foxglove_bridge's subprotocols, which miti_bridge refuses.
- Each message is copied once on its way from ROS to the clients.
- One executor thread serves subscriptions, service responses and the graph.
