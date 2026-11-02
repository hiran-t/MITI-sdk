// A libmiti server that offers what the conformance fixture graph offers (conformance/fixture),
// without ROS: the conformance suite checks libmiti against it.
//
//   conformance_server --share conformance/fixture/share [--port 8765]
//
// It is also the smallest complete example of libmiti: channels with ros2msg schemas and
// hand-written CDR, client publishing, a service, a parameter, assets and time.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <miti/server.hpp>

namespace
{

// ── ros2msg schemas: the type's definition, then each type it uses after a separator ──

const std::string SEP = "\n" + std::string(80, '=') + "\n";
const std::string TIME = "MSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec";
const std::string HEADER =
  "MSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id" + SEP + TIME;

const std::string STRING_SCHEMA = "string data";
const std::string IMAGE_SCHEMA =
  "std_msgs/Header header\nuint32 height\nuint32 width\nstring encoding\nuint8 is_bigendian\n"
  "uint32 step\nuint8[] data" + SEP + HEADER;
const std::string POINT_CLOUD2_SCHEMA =
  "std_msgs/Header header\nuint32 height\nuint32 width\nPointField[] fields\nbool is_bigendian\n"
  "uint32 point_step\nuint32 row_step\nuint8[] data\nbool is_dense" + SEP + HEADER + SEP +
  "MSG: sensor_msgs/PointField\nuint8 INT8=1\nuint8 UINT8=2\nuint8 INT16=3\nuint8 UINT16=4\n"
  "uint8 INT32=5\nuint8 UINT32=6\nuint8 FLOAT32=7\nuint8 FLOAT64=8\nstring name\nuint32 offset\n"
  "uint8 datatype\nuint32 count";
const std::string TF_MESSAGE_SCHEMA =
  "geometry_msgs/TransformStamped[] transforms" + SEP +
  "MSG: geometry_msgs/TransformStamped\nstd_msgs/Header header\nstring child_frame_id\n"
  "Transform transform" + SEP + HEADER + SEP +
  "MSG: geometry_msgs/Transform\nVector3 translation\nQuaternion rotation" + SEP +
  "MSG: geometry_msgs/Vector3\nfloat64 x\nfloat64 y\nfloat64 z" + SEP +
  "MSG: geometry_msgs/Quaternion\nfloat64 x 0\nfloat64 y 0\nfloat64 z 0\nfloat64 w 1";

const std::string ROBOT_DESCRIPTION =
  "<?xml version=\"1.0\"?>\n<robot name=\"conformance\">\n  <link name=\"base_link\">\n"
  "    <visual>\n      <geometry><mesh "
  "filename=\"package://miti_conformance_fixture/meshes/cube.stl\"/></geometry>\n"
  "    </visual>\n  </link>\n</robot>\n";

miti::MessageDefinition ros2msg(const std::string & name, const std::string & schema)
{
  return {"cdr", name, "ros2msg", schema};
}

/// Little-endian CDR, as ROS 2 writes it; alignment counts from after the 4-byte header.
class Cdr
{
public:
  Cdr & u8(uint8_t value) {bytes_.push_back(value); return *this;}
  Cdr & boolean(bool value) {return u8(value ? 1 : 0);}
  Cdr & i32(int32_t value) {return little(static_cast<uint32_t>(value), 4);}
  Cdr & u32(uint32_t value) {return little(value, 4);}
  Cdr & f32(float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return little(bits, 4);
  }
  Cdr & f64(double value)
  {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    return little(bits, 8);
  }
  Cdr & string(const std::string & text)
  {
    u32(static_cast<uint32_t>(text.size() + 1));
    bytes_.insert(bytes_.end(), text.begin(), text.end());
    return u8(0);
  }
  Cdr & bytes(const miti::Bytes & data)
  {
    u32(static_cast<uint32_t>(data.size()));
    bytes_.insert(bytes_.end(), data.begin(), data.end());
    return *this;
  }
  Cdr & header(miti::Timestamp stamp, const std::string & frame_id)
  {
    return i32(static_cast<int32_t>(stamp / 1000000000)).u32(stamp % 1000000000).string(frame_id);
  }
  miti::Bytes take() {return std::move(bytes_);}

private:
  Cdr & little(uint64_t value, size_t size)
  {
    while ((bytes_.size() - 4) % size) bytes_.push_back(0);
    for (size_t i = 0; i < size; ++i) bytes_.push_back(static_cast<uint8_t>(value >> (8 * i)));
    return *this;
  }

  miti::Bytes bytes_{0x00, 0x01, 0x00, 0x00};
};

miti::Timestamp now()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::atomic<bool> running{true};

}  // namespace

int main(int argc, char ** argv)
{
  std::string share;
  uint16_t port = 8765;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--share") share = argv[i + 1];
    if (flag == "--port") port = static_cast<uint16_t>(std::stoi(argv[i + 1]));
  }
  if (share.empty()) {
    std::cerr << "Usage: " << argv[0] << " --share conformance/fixture/share [--port 8765]\n";
    return 2;
  }

  std::shared_ptr<miti::Channel> echo;
  miti::ServerOptions options;
  options.name = "libmiti conformance server";
  options.port = port;
  options.services = true;
  options.time = true;
  options.client_publish.on_message =
    [&echo](const miti::ClientChannel & channel, const uint8_t * data, size_t size) {
      if (channel.topic == "/conformance/in") echo->log(miti::Bytes(data, data + size), now());
    };
  options.get_parameters = [](const std::vector<std::string> & names, auto reply) {
      std::vector<miti::Parameter> found;
      for (const auto & name : names) {
        if (name == "/conformance_fixture.robot_description") {
          found.push_back({name, ROBOT_DESCRIPTION});
        }
      }
      reply(std::move(found));
    };
  options.assets.packages["miti_conformance_fixture"] = share + "/miti_conformance_fixture";

  auto server = miti::Server::create(std::move(options));
  const auto string = server->add_channel(
    {"/conformance/string", ros2msg("std_msgs/msg/String", STRING_SCHEMA)});
  const auto image = server->add_channel(
    {"/conformance/image", ros2msg("sensor_msgs/msg/Image", IMAGE_SCHEMA)});
  const auto points = server->add_channel(
    {"/conformance/points", ros2msg("sensor_msgs/msg/PointCloud2", POINT_CLOUD2_SCHEMA)});
  const auto tf = server->add_channel(
    {"/tf", ros2msg("tf2_msgs/msg/TFMessage", TF_MESSAGE_SCHEMA)});
  echo = server->add_channel(
    {"/conformance/echo", ros2msg("std_msgs/msg/String", STRING_SCHEMA)});
  miti::ChannelSpec tf_static_spec{"/tf_static", ros2msg("tf2_msgs/msg/TFMessage", TF_MESSAGE_SCHEMA)};
  tf_static_spec.latched = 1;
  server->add_channel(tf_static_spec)->log(
    Cdr().u32(1).header(now(), "map").string("odom")
    .f64(0).f64(0).f64(0).f64(0).f64(0).f64(0).f64(1).take(),
    now());
  server->add_service(
    {"/conformance/set_bool", "std_srvs/srv/SetBool",
      ros2msg("std_srvs/srv/SetBool_Request", "bool data"),
      ros2msg("std_srvs/srv/SetBool_Response", "bool success\nstring message")},
    [](const miti::Bytes & request, miti::ServiceResponder responder) {
      if (request.size() < 5) {
        responder.fail("Expected a CDR std_srvs/srv/SetBool request");
        return;
      }
      const bool data = request[4] != 0;
      responder.respond(Cdr().boolean(true).string(data ? "set True" : "set False").take());
    });

  std::signal(SIGINT, [](int) {running = false;});
  std::signal(SIGTERM, [](int) {running = false;});
  std::cout << "libmiti conformance server on port " << server->port() << std::endl;

  for (uint64_t tick = 1; running; ++tick) {
    const auto stamp = now();
    server->broadcast_time(stamp);
    string->log(Cdr().string("tick " + std::to_string(tick)).take(), stamp);
    tf->log(
      Cdr().u32(1).header(stamp, "odom").string("base_link")
      .f64(1.5).f64(0).f64(0).f64(0).f64(0).f64(0).f64(1).take(),
      stamp);
    if (tick % 2 == 0) {
      miti::Bytes pixels(2 * 4 * 3);
      for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i);
      image->log(
        Cdr().header(stamp, "camera").u32(2).u32(4).string("rgb8").u8(0).u32(12).bytes(pixels)
        .take(),
        stamp);

      Cdr cloud;
      cloud.header(stamp, "base_link").u32(1).u32(3).u32(3);
      const char * names[] = {"x", "y", "z"};
      for (uint32_t i = 0; i < 3; ++i) cloud.string(names[i]).u32(4 * i).u8(7).u32(1);
      Cdr xyz;
      for (int i = 0; i < 3; ++i) xyz.f32(i).f32(2.0f * i).f32(3.0f * i);
      auto data = xyz.take();
      data.erase(data.begin(), data.begin() + 4);  // the points alone, without a CDR header
      cloud.boolean(false).u32(12).u32(36).bytes(data).boolean(true);
      points->log(cloud.take(), stamp);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
