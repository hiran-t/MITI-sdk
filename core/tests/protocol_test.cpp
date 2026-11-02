#include <gtest/gtest.h>

#include "protocol.hpp"

using miti::Bytes;
namespace protocol = miti::protocol;

TEST(Protocol, FindsTheSubprotocolAmongThoseOffered)
{
  EXPECT_TRUE(protocol::offers("miti.bridge.v1", "miti.bridge.v1"));
  EXPECT_TRUE(protocol::offers("foxglove.sdk.v1, miti.bridge.v1", "miti.bridge.v1"));
  EXPECT_TRUE(protocol::offers("miti.bridge.v1,foxglove.websocket.v1", "miti.bridge.v1"));
  EXPECT_FALSE(protocol::offers("miti.bridge.v10", "miti.bridge.v1"));
  EXPECT_FALSE(protocol::offers("foxglove.websocket.v1", "miti.bridge.v1"));
  EXPECT_FALSE(protocol::offers("", "miti.bridge.v1"));
}

TEST(Protocol, MessageDataHeaderIsLittleEndian)
{
  const auto header = protocol::message_data_header(0x01020304, 0x1122334455667788);
  EXPECT_EQ(
    (std::vector<uint8_t>(header.begin(), header.end())),
    (std::vector<uint8_t>{
    0x01, 0x04, 0x03, 0x02, 0x01, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}));
}

TEST(Protocol, ServiceCallResponseLayout)
{
  EXPECT_EQ(
    protocol::service_call_response(7, 9, "cdr", {0xaa}),
    (Bytes{0x03, 7, 0, 0, 0, 9, 0, 0, 0, 3, 0, 0, 0, 'c', 'd', 'r', 0xaa}));
}

TEST(Protocol, FetchAssetResponseCarriesDataOrError)
{
  EXPECT_EQ(
    protocol::fetch_asset_response(5, true, "ignored", {1, 2}),
    (Bytes{0x04, 5, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2}));
  EXPECT_EQ(
    protocol::fetch_asset_response(5, false, "no", {1, 2}),
    (Bytes{0x04, 5, 0, 0, 0, 1, 2, 0, 0, 0, 'n', 'o'}));
}

TEST(Protocol, ParsesClientMessageData)
{
  const Bytes frame{0x01, 3, 0, 0, 0, 'h', 'i'};
  const auto parsed = protocol::parse_client_binary(frame.data(), frame.size());
  const auto & message = std::get<protocol::ClientMessageData>(parsed);
  EXPECT_EQ(message.channel, 3u);
  EXPECT_EQ(std::string(message.data, message.data + message.size), "hi");
}

TEST(Protocol, ParsesServiceCallRequests)
{
  const Bytes frame{0x02, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 'c', 'd', 'r', 0xff};
  const auto parsed = protocol::parse_client_binary(frame.data(), frame.size());
  const auto & call = std::get<protocol::ServiceCallRequest>(parsed);
  EXPECT_EQ(call.service, 1u);
  EXPECT_EQ(call.call, 2u);
  EXPECT_EQ(call.encoding, "cdr");
  ASSERT_EQ(call.size, 1u);
  EXPECT_EQ(call.data[0], 0xff);
}

TEST(Protocol, RejectsMalformedClientFrames)
{
  const auto error = [](Bytes frame) {
      return std::holds_alternative<std::string>(
        protocol::parse_client_binary(frame.data(), frame.size()));
    };
  EXPECT_TRUE(error({}));
  EXPECT_TRUE(error({0x01, 1, 0}));
  EXPECT_TRUE(error({0x02, 1, 0, 0, 0, 2, 0, 0, 0, 9, 0, 0, 0, 'c'}));  // encoding cut off
  EXPECT_TRUE(error({0x07}));
}

TEST(Protocol, ParameterTypesThatJsonCannotTell)
{
  EXPECT_EQ(
    protocol::parameter({"/n.d", 1.0}),
    (nlohmann::json{{"name", "/n.d"}, {"value", 1.0}, {"type", "float64"}}));
  EXPECT_EQ(
    protocol::parameter({"/n.i", int64_t{1}}),
    (nlohmann::json{{"name", "/n.i"}, {"value", 1}}));
  EXPECT_EQ(
    protocol::parameter({"/n.b", std::vector<uint8_t>{1, 2, 3}})["value"], "AQID");
  EXPECT_EQ(protocol::parameter({"/n.b", std::vector<uint8_t>{1}})["value"], "AQ==");
  EXPECT_EQ(protocol::parameter({"/n.b", std::vector<uint8_t>{1, 2}})["value"], "AQI=");
  EXPECT_EQ(protocol::parameter({"/n.b", std::vector<uint8_t>{1}})["type"], "byte_array");
  EXPECT_EQ(
    protocol::parameter({"/n.a", std::vector<double>{1, 2}})["type"], "float64_array");
}
