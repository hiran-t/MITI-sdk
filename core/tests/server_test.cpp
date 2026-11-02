// The server over a real WebSocket, with a small blocking client.

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <chrono>
#include <future>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

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

/// A client that blocks, and fails a test instead of hanging when nothing arrives in time.
class Client
{
public:
  explicit Client(uint16_t port, const std::string & protocols = "miti.bridge.v1")
  : ws_(ioc_)
  {
    ws_.next_layer().connect({net::ip::make_address("127.0.0.1"), port});
    timeval timeout{5, 0};
    setsockopt(
      ws_.next_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
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
    beast::flat_buffer buffer;
    ws_.read(buffer);
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
  EXPECT_EQ(info["capabilities"], json::array({"time"}));
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
