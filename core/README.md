# libmiti

The C++ core of the MITI SDK. It serves MITI over WebSocket with [the MITI protocol](../docs/protocol.md), and has no ROS dependency. miti_bridge will be built on it, and so can any program that wants to stream to MITI.

```cpp
#include <miti/server.hpp>

miti::ServerOptions options;
options.name = "line-3";
options.port = 8765;
auto server = miti::Server::create(options);

auto status = server->add_channel(
  {"/robot/status", {"cdr", "acme_msgs/msg/Status", "ros2msg", status_definition}});
status->log(cdr_bytes, receive_time_ns);  // to every subscriber, shared, not copied
```

[`examples/conformance_server.cpp`](examples/conformance_server.cpp) uses everything libmiti offers. It is a server that offers what the conformance fixture graph offers, without ROS, so the conformance suite checks libmiti against it.

## Status: phases 1 to 3 of [the design](../docs/design.md)

| Done                                                                                                                                                       | Not yet                       |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------- |
| The base protocol: channels, latched channels, subscriptions, client publishing (CDR), services, parameters, assets (read off the server's thread), time   | Typed messages: phase 4       |
| `miti.subscriptionOptions`: `maxRate`, `delivery`, `priority` and `transform`, with `miti.subscriptionFormat`                                              | TLS and authentication: later |
| Transforms: `jpeg` (TurboJPEG) and `reduce` (point clouds)                                                                                                 |                               |
| `miti.getSchema` (the `get_schema` callback) and `miti.stats`                                                                                              |                               |
| The scheduler: control frames first, then priorities, round-robin, newest-wins for `latest`, a bounded queue for `all`, one frame in flight per connection |                               |
| Refusing handshakes that do not offer `miti.bridge.v1`                                                                                                     |                               |

Checked:

- **gtest:** 53 tests of the scheduler, the wire format, asset paths, the transforms, and the server over a real WebSocket; on Windows 56, with its path rules.
- **Conformance:** the suite passes against the example server, 33 checks, on Ubuntu and Windows. miti_bridge, built on libmiti, passes it on Humble and Jazzy.
- **Sanitizers:** clean under AddressSanitizer with UndefinedBehaviorSanitizer, and under ThreadSanitizer.
- **Toolchains:** builds without warnings, from Ubuntu 22.04 (GCC 11, Boost 1.74) to macOS (Clang, Boost 1.92) and Windows (MSVC at `/W4`, Boost 1.92). CI builds and checks it on Ubuntu 22.04 and 24.04, each on amd64 and arm64, and on Windows x64.

## How it runs

**Threads.** A server has one thread of its own, and everything it does runs there.

- Its methods, and a channel's `log`, may be called from any thread: they hand their work to that thread.
- Callbacks from `ServerOptions` run on that thread and must not block it. Hand longer work to another thread, and answer through the `ServiceResponder` or the parameter `reply` the callback was given. Both may be called from any thread.
- A service call that no one answers fails with "No response" once the last copy of its responder is gone.
- Transforms run on two threads of their own. Subscribers that ask for the same transform of a channel share one run, and a frame that arrives while the previous one is still being transformed waits in a single slot, where a newer frame replaces it.

**Sending.**

- Each connection writes one frame at a time. Messages wait in its scheduler, where a newer one can still replace them, and not in the socket: the socket holds at most 128 KB unsent (`TCP_NOTSENT_LOWAT`, on Linux and macOS), and sends small messages at once (`TCP_NODELAY`). Windows has no `TCP_NOTSENT_LOWAT`; there the send buffer follows the connection's ideal send backlog, about one bandwidth-delay product.
- A message's bytes are shared by every subscriber. Each connection writes only its own 13-byte header in front of them.

**Assets.**

- `package://name/...` is served from `AssetOptions::packages[name]`, and `file:///...` from inside `AssetOptions::roots`.
- `..` cannot lead out of a package or a root.
- Inside a package, symlinks are followed: `colcon build --symlink-install` installs every file as a link into the source tree. A root's files must really be inside it: there, symlinks are resolved before the check.
- Only allowed extensions are served, up to a size limit.
- On Windows, `file:///C:/dir/a.stl` names `C:\dir\a.stl`. A path whose root is not a drive, such as `\\host\share`, is refused before it is looked up, since looking would reach out to that host. A file's 8.3 short name, such as `MODEL~1.STL` for `model.stl_old`, does not make it an asset: its long name must have an allowed extension too.

## Build

With pixi, from the repository root (the environment brings CMake, a compiler, Boost, nlohmann/json, TurboJPEG and GoogleTest):

```bash
pixi run -e core test                  # build, then run the gtests
pixi run -e core conformance-libmiti   # the conformance suite against the example server
```

On Windows the same tasks build with MSVC from the pixi environment; Visual Studio 2022's C++ tools must be installed. Run `conformance-libmiti` from Git Bash.

Without pixi, install CMake, Ninja, pkg-config, a C++17 compiler, Boost 1.74 or newer, nlohmann/json 3.10 or newer, TurboJPEG and GoogleTest. On Ubuntu that is `cmake ninja-build pkg-config g++ libboost-dev nlohmann-json3-dev libturbojpeg0-dev libgtest-dev`. Then:

```bash
cmake -S core -B core/build -G Ninja && cmake --build core/build && ctest --test-dir core/build
```

```bash
cmake --install core/build --prefix /usr/local   # then find_package(libmiti) and link miti::miti
```

`package.xml` lets colcon and rosdep build libmiti in a ROS workspace as a plain CMake package.
