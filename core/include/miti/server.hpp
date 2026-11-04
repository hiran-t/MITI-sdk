// libmiti: serves MITI over WebSocket with the MITI protocol (docs/protocol.md).
//
// A program creates a Server, adds channels and logs messages to them; MITI subscribes to what it
// shows. Every public method may be called from any thread. Callbacks given in ServerOptions run
// on the server's own thread and must not block it: hand longer work to another thread and answer
// through the responder or reply they are given.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace miti
{

/// Nanoseconds: Unix time, or the robot's clock when it runs on simulation time.
using Timestamp = uint64_t;
using Bytes = std::vector<uint8_t>;

/// How a channel's or a service's messages are encoded (protocol.md, advertise).
struct MessageDefinition
{
  std::string encoding;         // "cdr", or "json"
  std::string schema_name;      // "sensor_msgs/msg/Image"
  std::string schema_encoding;  // "ros2msg"; empty for json without a schema
  std::string schema;           // the ros2msg text, with the definitions of the types it uses
};

/// A topic the server offers.
struct ChannelSpec
{
  std::string topic;
  MessageDefinition message;
  /// Keep this many of the newest messages and send them to each new subscriber first, as ROS
  /// does for transient-local topics such as /tf_static. 0 keeps none.
  size_t latched = 0;
};

namespace detail
{
struct ServerImpl;
}

/// A channel added to a server. Keep it to log messages; it stays on the server until removed.
class Channel
{
public:
  uint32_t id() const {return id_;}
  const ChannelSpec & spec() const {return spec_;}

  /// Sends a message to every subscriber. The bytes are shared by all of them, not copied.
  /// `receive_time` is when the message was received or made.
  void log(std::shared_ptr<const Bytes> message, Timestamp receive_time) const;
  void log(Bytes message, Timestamp receive_time) const;

  /// Whether any client is subscribed: a program can skip producing data nobody watches.
  bool has_subscribers() const {return subscribers_.load() > 0;}

private:
  friend struct detail::ServerImpl;
  Channel(uint32_t id, ChannelSpec spec, std::weak_ptr<detail::ServerImpl> server)
  : id_(id), spec_(std::move(spec)), server_(std::move(server)) {}

  uint32_t id_;
  ChannelSpec spec_;
  std::weak_ptr<detail::ServerImpl> server_;
  std::atomic<int> subscribers_{0};
};

/// A service the server offers.
struct ServiceSpec
{
  std::string name;  // "/reset"
  std::string type;  // "std_srvs/srv/Trigger"
  MessageDefinition request;
  MessageDefinition response;
};

/// Answers one service call, from any thread. The first answer counts; a call nobody answered
/// fails with "No response" when the last copy of its responder is gone.
class ServiceResponder
{
public:
  void respond(Bytes response) const;
  void fail(std::string message) const;

  struct State;
  explicit ServiceResponder(std::shared_ptr<State> state) : state_(std::move(state)) {}

private:
  std::shared_ptr<State> state_;
};

/// Called for each service call with the request in the service's request encoding.
using ServiceHandler = std::function<void (const Bytes & request, ServiceResponder responder)>;

/// A ROS parameter value. A double array is float64_array; integers stay integers.
using ParameterValue = std::variant<
  bool, int64_t, double, std::string, std::vector<uint8_t>, std::vector<bool>,
  std::vector<int64_t>, std::vector<double>, std::vector<std::string>>;

struct Parameter
{
  std::string name;  // "/node.parameter"
  ParameterValue value;
};

/// Called with the names a client asked for. Call `reply` once, from any thread, with the
/// parameters that exist; leave out the ones that do not.
using ParameterHandler = std::function<void (
      const std::vector<std::string> & names,
      std::function<void(std::vector<Parameter>)> reply)>;

/// A channel a client advertised to publish on.
struct ClientChannel
{
  uint64_t client;  // the connection, unique for the server's lifetime
  uint32_t id;      // the client's id for the channel
  std::string topic;
  MessageDefinition message;
};

/// Client publishing. Setting `on_message` lets clients publish (the clientPublish capability).
struct ClientPublishHandlers
{
  std::function<void(const ClientChannel &)> on_advertise;
  std::function<void(const ClientChannel &, const uint8_t * data, size_t size)> on_message;
  /// Also called for every channel of a client that disconnects.
  std::function<void(const ClientChannel &)> on_unadvertise;
};

/// Files clients may fetch (the assets capability, on when any package or root is set).
struct AssetOptions
{
  /// `package://name/path` is served from inside packages[name]. `..` cannot lead out of it, and
  /// symlinks inside it are followed, as `colcon build --symlink-install` installs every file as
  /// one.
  std::map<std::string, std::filesystem::path> packages;
  /// `file:///path` is served when the file, symlinks resolved, is inside one of these. On Windows
  /// it is `file:///C:/path`; paths that are not on a drive are refused.
  std::vector<std::filesystem::path> roots;
  /// Lowercase, with the dot.
  std::vector<std::string> extensions{
    ".urdf", ".dae", ".stl", ".obj", ".glb", ".gltf", ".mtl", ".png", ".jpg", ".jpeg"};
  uintmax_t max_size = 64u << 20;
};

struct ServerOptions
{
  std::string name = "libmiti";
  std::string address = "0.0.0.0";
  uint16_t port = 8765;  // 0 takes a free port; Server::port() tells which
  std::map<std::string, std::string> metadata;

  /// Messages an `all` subscription may have waiting before the oldest is dropped.
  size_t queue_limit = 100;

  /// Offer services (add them with Server::add_service).
  bool services = false;
  /// The server sends time (call Server::broadcast_time).
  bool time = false;
  ClientPublishHandlers client_publish;
  /// Set to offer parameters.
  ParameterHandler get_parameters;
  /// The definition of a message type ("pkg/msg/Type"), or none when it is unknown. Set it to
  /// answer miti.getSchema (the miti.schemas capability), so clients can encode what they publish.
  std::function<std::optional<MessageDefinition>(const std::string & type)> get_schema;
  AssetOptions assets;

  /// A channel got its first subscriber, or lost its last.
  std::function<void(const Channel &)> on_subscribe;
  std::function<void(const Channel &)> on_unsubscribe;
};

class Server
{
public:
  /// Starts listening right away; throws std::system_error when the address cannot be bound.
  static std::unique_ptr<Server> create(ServerOptions options);
  ~Server();
  Server(const Server &) = delete;
  Server & operator=(const Server &) = delete;

  uint16_t port() const;

  std::shared_ptr<Channel> add_channel(ChannelSpec spec);
  void remove_channel(const Channel & channel);

  /// Needs ServerOptions::services. Returns the service's id.
  uint32_t add_service(ServiceSpec spec, ServiceHandler handler);
  void remove_service(uint32_t id);

  /// Needs ServerOptions::time.
  void broadcast_time(Timestamp time);

  /// Closes every connection and stops. The destructor does this too.
  void stop();

private:
  explicit Server(std::shared_ptr<detail::ServerImpl> impl);
  std::shared_ptr<detail::ServerImpl> impl_;
};

}  // namespace miti
