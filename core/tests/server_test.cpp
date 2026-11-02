// The server over a real WebSocket, with a small blocking client.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "cdr.hpp"
#include "miti/server.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using json = nlohmann::json;
using miti::Bytes;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

namespace
{

const miti::MessageDefinition STRING{"cdr", "std_msgs/msg/String", "ros2msg", "string data"};
const miti::MessageDefinition IMAGE{"cdr", "sensor_msgs/Image", "ros2msg", "uint32 height"};

/// A 4 by 2 sensor_msgs/Image.
Bytes image(const std::string & encoding)
{
  const Bytes pixels(4 * 2 * 3, 0x80);
  miti::cdr::Writer writer;
  miti::cdr::write_header(writer, {1, 2, "camera"});
  writer.u32(2).u32(4).string(encoding).u8(0).u32(12).bytes(pixels.data(), pixels.size());
  return writer.take();
}

/// A client that blocks, and fails a test instead of hanging when nothing arrives in time.
class Client
{
public:
  explicit Client(uint16_t port, const std::string & protocols = "miti.bridge.v1")
  : ws_(ioc_)
  {
    ws_.next_layer().connect({net::ip::make_address("127.0.0.1"), port});
    ws_.set_option(websocket::stream_base::decorator(
      [protocols](websocket::request_type & request) {
        request.set(beast::http::field::sec_websocket_protocol, protocols);
      }));
    ws_.handshake("127.0.0.1", "/");
  }

  struct Frame
  {
    bool text;
    std::string data;
    json op() const {return json::parse(data);}
    Bytes bytes() const {return Bytes(data.begin(), data.end());}
  };

  Frame next()
  {
    // Asio's blocking reads wait past SO_RCVTIMEO, so read asynchronously with a deadline.
    beast::flat_buffer buffer;
    std::optional<beast::error_code> result;
    ws_.async_read(buffer, [&result](beast::error_code error, size_t) {result = error;});
    ioc_.restart();
    ioc_.run_for(5s);
    if (!result) {
      ws_.next_layer().cancel();
      ioc_.restart();
      ioc_.run();
      throw std::runtime_error("Nothing arrived within 5 s");
    }
    if (*result) throw beast::system_error(*result);
    return {ws_.got_text(), beast::buffers_to_string(buffer.data())};
  }

  /// The next JSON message with this op, skipping the others.
  json next(const std::string & op)
  {
    for (;;) {
      const auto frame = next();
      if (frame.text && frame.op()["op"] == op) return frame.op();
    }
  }

  /// The next binary frame with this opcode, skipping the others.
  Bytes next_binary(uint8_t opcode)
  {
    for (;;) {
      const auto frame = next();
      if (!frame.text && !frame.data.empty() && static_cast<uint8_t>(frame.data[0]) == opcode) {
        return frame.bytes();
      }
    }
  }

  void send(const json & message)
  {
    ws_.text(true);
    ws_.write(net::buffer(message.dump()));
  }

  void send(const Bytes & frame)
  {
    ws_.binary(true);
    ws_.write(net::buffer(frame));
  }

private:
  net::io_context ioc_;
  websocket::stream<tcp::socket> ws_;
};

miti::ServerOptions local()
{
  miti::ServerOptions options;
  options.address = "127.0.0.1";
  options.port = 0;
  return options;
}

/// Waits for a condition the server's thread makes true.
template<typename Predicate>
bool eventually(Predicate predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

uint32_t u32(const Bytes & bytes, size_t offset)
{
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes.at(offset + i)) << (8 * i);
  return value;
}

}  // namespace

TEST(Server, RefusesClientsThatDoNotOfferTheMitiSubprotocol)
{
  const auto server = miti::Server::create(local());
  EXPECT_THROW(Client(server->port(), "foxglove.websocket.v1"), beast::system_error);
  Client accepted(server->port(), "foxglove.sdk.v1, miti.bridge.v1");
  EXPECT_EQ(accepted.next("serverInfo")["name"], "libmiti");
}

TEST(Server, SendsServerInfoThenItsChannels)
{
  auto options = local();
  options.name = "test";
  options.time = true;
  options.metadata["robot"] = "r1";
  const auto server = miti::Server::create(options);
  const auto channel = server->add_channel({"/chatter", STRING});

  Client client(server->port());
  const auto info = client.next().op();
  EXPECT_EQ(info["op"], "serverInfo");
  EXPECT_EQ(info["name"], "test");
  EXPECT_EQ(info["capabilities"], json::array({"time", "miti.transforms", "miti.stats"}));
  EXPECT_EQ(info["metadata"]["robot"], "r1");
  EXPECT_TRUE(info["metadata"].contains("miti.version"));

  const auto advertise = client.next().op();
  EXPECT_EQ(advertise["op"], "advertise");
  ASSERT_EQ(advertise["channels"].size(), 1u);
  EXPECT_EQ(advertise["channels"][0]["id"], channel->id());
  EXPECT_EQ(advertise["channels"][0]["topic"], "/chatter");
  EXPECT_EQ(advertise["channels"][0]["schemaEncoding"], "ros2msg");
}

TEST(Server, DeliversLoggedMessagesToSubscribers)
{
  std::atomic<int> subscribed{0};
  std::atomic<int> unsubscribed{0};
  auto options = local();
  options.on_subscribe = [&](const miti::Channel &) {++subscribed;};
  options.on_unsubscribe = [&](const miti::Channel &) {++unsubscribed;};
  const auto server = miti::Server::create(options);
  const auto channel = server->add_channel({"/chatter", STRING});

  Client client(server->port());
  client.next("advertise");
  client.send(json{{"op", "subscribe"}, {"subscriptions", {{{"id", 7}, {"channelId", channel->id()}}}}});
  ASSERT_TRUE(eventually([&] {return channel->has_subscribers();}));
  EXPECT_EQ(subscribed, 1);

  channel->log(Bytes{0xa, 0xb}, 42);
  const auto frame = client.next_binary(0x01);
  EXPECT_EQ(
    frame, (Bytes{0x01, 7, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 0xa, 0xb}));

  client.send(json{{"op", "unsubscribe"}, {"subscriptionIds", {7}}});
  ASSERT_TRUE(eventually([&] {return !channel->has_subscribers();}));
  EXPECT_EQ(unsubscribed, 1);
}

TEST(Server, SendsLatchedMessagesToEachNewSubscriber)
{
  const auto server = miti::Server::create(local());
  miti::ChannelSpec spec{"/tf_static", STRING};
  spec.latched = 2;
  const auto channel = server->add_channel(spec);
  // Logged before anyone subscribes: only the newest two are kept.
  for (uint8_t i = 1; i <= 3; ++i) channel->log(Bytes{i}, i);

  for (uint32_t subscription : {5u, 6u}) {
    Client client(server->port());
    client.next("advertise");
    client.send(json{
        {"op", "subscribe"},
        {"subscriptions", {{{"id", subscription}, {"channelId", channel->id()}}}}});
    const auto first = client.next_binary(0x01);
    EXPECT_EQ(u32(first, 1), subscription);
    EXPECT_EQ(first.back(), 2);
    EXPECT_EQ(client.next_binary(0x01).back(), 3);
  }
}

TEST(Server, AnswersMistakesWithStatusErrors)
{
  const auto server = miti::Server::create(local());
  Client client(server->port());
  client.send(json{{"op", "subscribe"}, {"subscriptions", {{{"id", 1}, {"channelId", 99}}}}});
  EXPECT_EQ(client.next("status")["level"], 2);
  client.send(json{{"op", "getParameters"}, {"parameterNames", {"/a.b"}}});
  EXPECT_EQ(client.next("status")["level"], 2);  // no parameters capability
  client.send(json{{"op", "subscribe"}});
  EXPECT_EQ(client.next("status")["level"], 2);  // no subscriptions field
  client.send(json{{"op", "dance"}});
  EXPECT_EQ(client.next("status")["level"], 1);  // unknown op: a warning
}

TEST(Server, AdvertisesAndUnadvertisesChannelsAddedLater)
{
  const auto server = miti::Server::create(local());
  Client client(server->port());
  client.next("serverInfo");
  const auto channel = server->add_channel({"/later", STRING});
  EXPECT_EQ(client.next("advertise")["channels"][0]["topic"], "/later");
  server->remove_channel(*channel);
  EXPECT_EQ(client.next("unadvertise")["channelIds"], json::array({channel->id()}));
}

TEST(Server, CallsServicesAndReturnsTheirResponses)
{
  auto options = local();
  options.services = true;
  const auto server = miti::Server::create(options);
  const miti::MessageDefinition cdr{"cdr", "x", "ros2msg", ""};
  const auto echo = server->add_service(
    {"/echo", "test_srvs/srv/Echo", cdr, cdr},
    [](const Bytes & request, miti::ServiceResponder responder) {responder.respond(request);});
  const auto silent = server->add_service(
    {"/silent", "test_srvs/srv/Echo", cdr, cdr}, [](const Bytes &, miti::ServiceResponder) {});

  Client client(server->port());
  const auto services = client.next("advertiseServices")["services"];
  EXPECT_EQ(services[0]["name"], "/echo");
  EXPECT_EQ(services[0]["request"]["encoding"], "cdr");

  const auto call = [&](uint32_t service, uint32_t id) {
      Bytes frame{0x02};
      for (const uint32_t value : {service, id, 3u}) {
        for (int i = 0; i < 4; ++i) frame.push_back(static_cast<uint8_t>(value >> (8 * i)));
      }
      frame.insert(frame.end(), {'c', 'd', 'r', 0x5});
      client.send(frame);
    };
  call(echo, 11);
  const auto response = client.next_binary(0x03);
  EXPECT_EQ(u32(response, 1), echo);
  EXPECT_EQ(u32(response, 5), 11u);
  EXPECT_EQ(response.back(), 0x5);

  // A handler that drops its responder without answering: the call still gets an answer.
  call(silent, 12);
  const auto failure = client.next("serviceCallFailure");
  EXPECT_EQ(failure["callId"], 12);
  EXPECT_EQ(failure["message"], "No response");

  call(9999, 13);
  EXPECT_EQ(client.next("serviceCallFailure")["callId"], 13);
}

TEST(Server, PassesPublishedMessagesToTheProgram)
{
  std::promise<std::pair<std::string, Bytes>> published;
  auto options = local();
  options.client_publish.on_message =
    [&](const miti::ClientChannel & channel, const uint8_t * data, size_t size) {
      published.set_value({channel.topic, Bytes(data, data + size)});
    };
  const auto server = miti::Server::create(options);
  Client client(server->port());
  EXPECT_EQ(client.next("serverInfo")["supportedEncodings"], json::array({"cdr"}));

  client.send(json{{"op", "advertise"}, {"channels", {{
        {"id", 3}, {"topic", "/in"}, {"encoding", "cdr"}, {"schemaName", "std_msgs/msg/String"}}}}});
  client.send(Bytes{0x01, 3, 0, 0, 0, 0xc, 0xd});
  auto result = published.get_future();
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  const auto [topic, data] = result.get();
  EXPECT_EQ(topic, "/in");
  EXPECT_EQ(data, (Bytes{0xc, 0xd}));

  client.send(json{{"op", "advertise"}, {"channels", {{
        {"id", 4}, {"topic", "/in"}, {"encoding", "json"}, {"schemaName", "std_msgs/msg/String"}}}}});
  EXPECT_EQ(client.next("status")["level"], 2);  // cdr only
}

TEST(Server, RepliesToParameterRequestsWithTheirId)
{
  auto options = local();
  options.get_parameters = [](const std::vector<std::string> & names, auto reply) {
      std::vector<miti::Parameter> found;
      for (const auto & name : names) {
        if (name == "/robot.name") found.push_back({name, std::string("r1")});
      }
      // Answering later, from another thread, is allowed.
      std::thread([reply, found] {reply(found);}).detach();
    };
  const auto server = miti::Server::create(options);
  Client client(server->port());
  client.send(json{
      {"op", "getParameters"}, {"parameterNames", {"/robot.name", "/robot.missing"}},
      {"id", "p1"}});
  const auto values = client.next("parameterValues");
  EXPECT_EQ(values["id"], "p1");
  EXPECT_EQ(values["parameters"], json::parse(R"([{"name": "/robot.name", "value": "r1"}])"));
}

TEST(Server, TransformsMessagesAndSaysWhenTheirFormatChanges)
{
  const auto server = miti::Server::create(local());
  const auto camera = server->add_channel({"/camera", IMAGE});
  const auto chatter = server->add_channel({"/chatter", STRING});
  Client client(server->port());
  client.next("advertise");
  client.send(json{{"op", "subscribe"}, {"subscriptions", {
        {{"id", 1}, {"channelId", camera->id()}}, {{"id", 2}, {"channelId", chatter->id()}}}}});
  ASSERT_TRUE(eventually([&] {return camera->has_subscribers() && chatter->has_subscribers();}));

  client.send(json{{"op", "miti.subscriptionOptions"}, {"subscriptionId", 2},
      {"transform", {{"kind", "jpeg"}}}});
  EXPECT_EQ(client.next("status")["level"], 2);  // not an image topic

  client.send(json{{"op", "miti.subscriptionOptions"}, {"subscriptionId", 1},
      {"transform", {{"kind", "jpeg"}, {"quality", 50}}}});
  const auto format = client.next("miti.subscriptionFormat");
  EXPECT_EQ(format["subscriptionId"], 1);
  EXPECT_EQ(format["encoding"], "cdr");
  EXPECT_EQ(format["schemaName"], "sensor_msgs/msg/CompressedImage");
  EXPECT_EQ(format["schemaEncoding"], "ros2msg");

  camera->log(image("rgb8"), 7);
  const auto frame = client.next_binary(0x01);
  EXPECT_EQ(u32(frame, 1), 1u);
  miti::cdr::Reader reader(frame.data() + 13, frame.size() - 13);
  EXPECT_EQ(miti::cdr::read_header(reader).frame_id, "camera");
  EXPECT_EQ(reader.string(), "jpeg");
  const auto [jpeg, size] = reader.bytes();
  ASSERT_GE(size, 2u);
  EXPECT_EQ(jpeg[0], 0xff);
  EXPECT_EQ(jpeg[1], 0xd8);

  // One warning for messages the transform cannot make, however many there are.
  camera->log(image("16UC1"), 8);
  camera->log(image("16UC1"), 9);
  EXPECT_EQ(client.next("status")["level"], 1);
  camera->log(image("bgr8"), 10);
  const auto next = client.next();
  EXPECT_FALSE(next.text) << next.data;

  client.send(json{{"op", "miti.subscriptionOptions"}, {"subscriptionId", 1},
      {"transform", nullptr}});
  EXPECT_EQ(client.next("miti.subscriptionFormat")["schemaName"], "sensor_msgs/Image");
  const auto raw = image("rgb8");
  camera->log(raw, 11);
  const auto untouched = client.next_binary(0x01);
  EXPECT_EQ(Bytes(untouched.begin() + 13, untouched.end()), raw);
}

TEST(Server, SendsStatsWhileAClientWantsThem)
{
  const auto server = miti::Server::create(local());
  const auto channel = server->add_channel({"/chatter", STRING});
  Client client(server->port());
  client.next("advertise");
  client.send(json{{"op", "miti.subscribeStats"}, {"intervalMs", 100}});
  EXPECT_EQ(client.next("status")["level"], 2);

  client.send(json{{"op", "subscribe"}, {"subscriptions", {{{"id", 3}, {"channelId", channel->id()}}}}});
  ASSERT_TRUE(eventually([&] {return channel->has_subscribers();}));
  client.send(json{{"op", "miti.subscribeStats"}, {"intervalMs", 250}});
  // Counts start when the server handles miti.subscribeStats: log after its first stats.
  client.next("miti.stats");
  channel->log(Bytes(100), 1);
  json stats;
  for (int i = 0; i < 8; ++i) {
    stats = client.next("miti.stats");
    if (stats["subscriptions"][0]["sent"] == 1) break;
  }
  ASSERT_EQ(stats["subscriptions"][0]["sent"], 1);
  EXPECT_GT(stats["timestamp"].get<int64_t>(), 1700000000000);
  EXPECT_EQ(stats["bufferedBytes"], 0);
  const auto & subscription = stats["subscriptions"][0];
  EXPECT_EQ(subscription["subscriptionId"], 3);
  EXPECT_EQ(subscription["dropped"], 0);
  EXPECT_GT(subscription["bytesPerSecond"].get<double>(), 0);
  EXPECT_EQ(subscription["oldestQueuedMs"], 0);

  client.send(json{{"op", "miti.unsubscribeStats"}});
  client.send(json{{"op", "dance"}});  // answered after the unsubscribe is handled
  client.next("status");
  // Two intervals with no stats.
  std::this_thread::sleep_for(600ms);
  channel->log(Bytes{1}, 2);
  const auto after = client.next();
  EXPECT_FALSE(after.text) << after.data;
}

TEST(Server, AFrameReplacedBeforeItsTransformGoesToEveryoneWhoWaitedForIt)
{
  const auto server = miti::Server::create(local());
  const auto camera = server->add_channel({"/camera", IMAGE});
  Client fast(server->port());
  Client capped(server->port());
  for (auto * client : {&fast, &capped}) {
    client->next("advertise");
    client->send(json{{"op", "subscribe"}, {"subscriptions", {{{"id", 1}, {"channelId", camera->id()}}}}});
  }
  ASSERT_TRUE(eventually([&] {return camera->has_subscribers();}));
  fast.send(json{{"op", "miti.subscriptionOptions"}, {"subscriptionId", 1},
      {"transform", {{"kind", "jpeg"}}}});
  capped.send(json{{"op", "miti.subscriptionOptions"}, {"subscriptionId", 1}, {"maxRate", 1000},
      {"transform", {{"kind", "jpeg"}}}});
  fast.next("miti.subscriptionFormat");
  capped.next("miti.subscriptionFormat");

  // A large frame keeps the transform busy (tens of ms) while the next two arrive. The capped
  // client is let through for the first of them, 2 ms later, but not for the second, right after
  // it. The second replaces the first before it is transformed, and the capped client gets it.
  const uint32_t width = 4000, height = 3000;
  const Bytes pixels(size_t(width) * height * 3, 0x80);
  miti::cdr::Writer large;
  miti::cdr::write_header(large, {1, 2, "camera"});
  large.u32(height).u32(width).string("rgb8").u8(0).u32(width * 3).bytes(pixels.data(), pixels.size());
  camera->log(large.take(), 1);
  std::this_thread::sleep_for(2ms);
  camera->log(image("rgb8"), 2);
  camera->log(image("rgb8"), 3);
  for (auto * client : {&fast, &capped}) {
    EXPECT_EQ(client->next_binary(0x01).at(5), 1);  // the receive time's low byte
    EXPECT_EQ(client->next_binary(0x01).at(5), 3);
  }
}
