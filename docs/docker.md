# miti_bridge with Docker

The Docker image `ghcr.io/hiran-t/miti-bridge` runs miti_bridge as a service, on the robot or on a computer on the robot's network. It holds ROS 2, miti_bridge from the release's packages, and the message packages a robot usually publishes (`common_interfaces`, TF, Nav2, maps, MoveIt, ros2_control, vision). It is built for PCs (amd64) and ARM computers such as NVIDIA Jetson (arm64).

| Tag            | What it is                                      |
| -------------- | ----------------------------------------------- |
| `humble`       | the newest release, on ROS 2 Humble             |
| `jazzy`        | the newest release, on ROS 2 Jazzy              |
| `0.1.2-humble` | that release on Humble; also for other versions |
| `0.1.2-jazzy`  | that release on Jazzy                           |

Use the ROS distro the robot runs: Humble and Jazzy nodes do not discover each other reliably.

## 1. Install Docker

```bash
curl -fsSL https://get.docker.com | sudo sh
```

```bash
sudo usermod -aG docker $USER
```

Log out and log in again, so `docker` runs without `sudo`. If Docker is already installed, skip this step.

## 2. Start miti_bridge

Get the Compose file and write its settings, here for a robot on Jazzy, in ROS domain 0:

```bash
mkdir -p ~/miti-bridge && cd ~/miti-bridge
curl -fsSL https://raw.githubusercontent.com/hiran-t/MITI-sdk/main/docker/docker-compose.yml -o docker-compose.yml
printf 'MITI_BRIDGE_TAG=jazzy\nROS_DOMAIN_ID=0\n' > .env
```

```bash
docker compose up -d
```

The service starts again with the machine (`restart: unless-stopped`) once Docker starts at boot. It uses the host's network, so DDS finds the robot's nodes and MITI reaches the bridge on the machine's own address.

## 3. Check it

```bash
docker compose logs -f
```

The log ends with `Serving MITI on port 8765 (miti.bridge.v1)`. In MITI, enter `ws://<this machine's address>:8765` as the **Bridge URL** with the gear in the status bar.

## The robot's own messages and meshes

The image knows the standard message types. For the robot's own message packages, and for the `package://` meshes of its robot description, mount the robot's colcon workspace at `/overlay`: uncomment `volumes` in `docker-compose.yml` and set the path. The workspace must be built for the same ROS distro and architecture, as it is on the robot; the bridge sources its `install/setup.bash`.

```yaml
volumes:
  - /home/robot/ros2_ws:/overlay:ro
```

Then `docker compose up -d` again.

## Settings

In `.env`, next to `docker-compose.yml`:

| Variable             | Default            | What it sets                                            |
| -------------------- | ------------------ | ------------------------------------------------------- |
| `MITI_BRIDGE_TAG`    | `jazzy`            | the image tag, as in the table above                    |
| `ROS_DOMAIN_ID`      | `0`                | the robot's ROS domain                                  |
| `RMW_IMPLEMENTATION` | `rmw_fastrtps_cpp` | the robot's middleware; `rmw_cyclonedds_cpp` also works |
| `MITI_BRIDGE_PORT`   | `8765`             | the WebSocket port                                      |

Other arguments of `ros2 launch miti_bridge miti_start` go in the service's `command`, for example to offer only some topics:

```yaml
command: ["topic_whitelist:=['/tf.*', '/camera/.*']"]
```

## Update

```bash
docker compose pull && docker compose up -d
```

To stay on one release, set `MITI_BRIDGE_TAG` to its version, such as `0.1.2-jazzy`.

## Without Compose

```bash
docker run -d --name miti-bridge --restart unless-stopped --network host --ipc host \
  -e ROS_DOMAIN_ID=0 ghcr.io/hiran-t/miti-bridge:jazzy
```

## Build the image yourself

`docker/Dockerfile` installs the `.deb` packages from `dist/`, for example those of a release:

```bash
gh release download v0.1.2 --repo hiran-t/MITI-sdk --pattern 'ros-jazzy-*' --dir dist
docker build -f docker/Dockerfile --build-arg ROS_DISTRO=jazzy -t miti-bridge:jazzy .
```

The Packages workflow builds and starts the image for every change, and pushes it for every release.
