# MITI SDK

The robot side of [MITI](https://github.com/hiran-t/MITI). **miti_bridge** connects a ROS 2 robot to MITI. It is built on **libmiti**, a C++ library that any program can use to stream to MITI, with or without ROS.

## Install miti_bridge

On the robot: Ubuntu 22.04 (Humble) or 24.04 (Jazzy), amd64 or arm64, with ROS 2 installed. miti_bridge is not in ROS's apt repository yet, so add this repository's own apt repository once, then install the package:

```bash
curl -fsSL https://hiran-t.github.io/MITI-sdk/key.gpg | sudo gpg --dearmor -o /usr/share/keyrings/miti-sdk.gpg
```

```bash
echo "deb [signed-by=/usr/share/keyrings/miti-sdk.gpg] https://hiran-t.github.io/MITI-sdk $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | sudo tee /etc/apt/sources.list.d/miti-sdk.list
```

```bash
sudo apt update && sudo apt install ros-$ROS_DISTRO-miti-bridge
```

To run it as a service with Docker, on the robot or on a computer on its network, use the image `ghcr.io/hiran-t/miti-bridge` (Humble or Jazzy, amd64 or arm64); [docs/docker.md](docs/docker.md) has the steps:

```bash
docker run -d --name miti-bridge --restart unless-stopped --network host --ipc host \
  -e ROS_DOMAIN_ID=0 ghcr.io/hiran-t/miti-bridge:jazzy
```

On Windows, [build it with pixi](#windows). On another setup, [build it from source](#build-from-source). The packages are also on their way into ROS's own repositories ([docs/releases.md](docs/releases.md#the-ros-buildfarm)), which will need no repository to add.

## Run

```bash
ros2 launch miti_bridge miti_start
```

It serves MITI on port 8765. In MITI, click the gear in the status bar and enter `ws://<robot-ip>:8765` as the **Bridge URL**. Every [parameter](ros/miti_bridge/README.md#parameters) is also a launch argument, for example:

```bash
ros2 launch miti_bridge miti_start port:=8765 "topic_whitelist:=['/tf.*', '/camera/.*']"
```

To keep the bridge's work off the robot, it can also run on the MITI machine and join the robot's ROS 2 network over DDS: see [Run miti_bridge beside MITI](https://miti.readthedocs.io/en/latest/how-to-guides/run-miti-bridge-beside-miti.html) in MITI's manual.

## Build from source

On Ubuntu with ROS 2 installed, in a folder of your choice:

```bash
sudo apt install python3-rosdep python3-colcon-common-extensions git
git clone https://github.com/hiran-t/MITI-sdk.git && cd MITI-sdk
source /opt/ros/$ROS_DISTRO/setup.bash
sudo rosdep init   # once per machine, if rosdep was never set up
rosdep update && rosdep install --from-paths core ros --ignore-src -y
colcon build --base-paths core ros --cmake-args -DCMAKE_BUILD_TYPE=Release -DMITI_BUILD_TESTS=OFF -DMITI_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF
```

Then run it from that folder:

```bash
source install/setup.bash
ros2 launch miti_bridge miti_start
```

### Windows

There is no package for Windows: miti_bridge is built from source with [pixi](https://pixi.sh), which brings ROS 2 Humble or Jazzy from [RoboStack](https://robostack.github.io) and the libraries libmiti needs. It needs [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) or its Build Tools, with the **Desktop development with C++** workload, and [Git](https://git-scm.com/downloads/win). In PowerShell:

```powershell
git clone https://github.com/hiran-t/MITI-sdk.git; cd MITI-sdk
pixi run -e humble build-bridge   # into install\humble; -e jazzy for Jazzy
```

Then run it from that folder:

```powershell
pixi shell -e humble
.\install\humble\local_setup.ps1
ros2 launch miti_bridge miti_start
```

If PowerShell refuses to run `local_setup.ps1`, allow your own scripts once with `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`. When Windows asks whether miti_bridge may use the network, allow it on private networks for MITI on another machine to connect.

## Status

libmiti, and miti_bridge on Humble and Jazzy, pass the conformance suite. MITI connects to miti_bridge from 3.4.3, and has it send cameras as JPEG and reduce point clouds on the robot. Typed messages are planned.

Where to read more:

- [docs/design.md](docs/design.md): goals, architecture, measurements and decisions
- [docs/protocol.md](docs/protocol.md): the MITI protocol, version 1 (`miti.bridge.v1`)
- [conformance/](conformance/README.md): checks a server against the protocol, on a fixture ROS 2 graph
- [core/](core/README.md): libmiti, the C++ core
- [ros/miti_bridge/](ros/miti_bridge/README.md): the ROS 2 node, its parameters and how it works
- [docs/releases.md](docs/releases.md): how releases and the apt repository are made
- [prototypes/generic_service_client/](prototypes/generic_service_client/README.md): calling a service of any type on Humble, which miti_bridge needs
- [bench/link/](bench/link/run.sh): how late TF and a camera arrive over a slow link (`tc netem`)

| Part          | What it is                                                                                                        |
| ------------- | ----------------------------------------------------------------------------------------------------------------- |
| `libmiti`     | The C++ core, without ROS: WebSocket server, priority scheduling, JPEG and point cloud reduction                  |
| `miti_bridge` | A ROS 2 node (Humble, Jazzy) on `libmiti`, which serves a robot's topics, services, parameters and meshes to MITI |

The bridge sends less over slow links, decided on the robot before data leaves it: rate limits, newest-frame delivery for cameras and point clouds, priority for TF, JPEG for raw images and point cloud reduction.

Panels for MITI are built with the widget SDK, [MITI-widget-sdk](https://github.com/hiran-t/MITI-widget-sdk), which was in this repository until 2026-11-02.

## Develop

[pixi](https://pixi.sh) brings ROS 2, a C++ toolchain and Node.js, so no system ROS install is needed (Linux, macOS arm64 and Windows x64; on Windows, run the conformance tasks from Git Bash):

```bash
pixi run -e humble service-prototype      # build the service-client prototype and call the fixture
pixi run -e core test                     # build libmiti and run its tests (no ROS needed)
pixi run -e core conformance-libmiti      # the conformance suite against libmiti's example server
pixi run -e humble build-bridge           # libmiti and miti_bridge with colcon, into install/humble (-e jazzy too)
pixi run -e humble test-bridge            # their tests
pixi run -e humble conformance-bridge     # the conformance suite against miti_bridge and the fixture graph
pixi run type-check                       # type-check the conformance suite
```

CI (`.github/workflows/ci.yml`) runs the same, on every supported Ubuntu (22.04 and 24.04) and on both amd64 and arm64:

- libmiti on each of the four, and once more with sanitizers.
- The ROS checks in `ros:<distro>-ros-base` containers: Humble on 22.04, Jazzy on 24.04, each on both architectures.
- On Windows x64, with MSVC and the pixi environments: libmiti, and miti_bridge on RoboStack's Humble and Jazzy.

## License

[MIT](LICENSE).
