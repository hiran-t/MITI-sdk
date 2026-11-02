// The wire format of docs/protocol.md: JSON operations and binary frames.

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "miti/server.hpp"

namespace miti::protocol
{

// A C string: Beast takes it on every Boost version. Older ones, such as Ubuntu 22.04's 1.74, have
// a string_view of their own that std::string_view does not convert to.
inline constexpr char SUBPROTOCOL[] = "miti.bridge.v1";

enum class StatusLevel { info = 0, warning = 1, error = 2 };

/// Does a Sec-WebSocket-Protocol header ("a, b") offer `protocol`?
bool offers(std::string_view header, std::string_view protocol);

nlohmann::json channel(const Channel & channel);
nlohmann::json service(uint32_t id, const ServiceSpec & spec);
nlohmann::json parameter(const Parameter & parameter);
std::string status(StatusLevel level, const std::string & message);

// ── Server → client binary frames ──

/// Opcode 0x01 without the message: the message bytes follow it in the same frame.
std::array<uint8_t, 13> message_data_header(uint32_t subscription, Timestamp time);
Bytes time(Timestamp time);
Bytes service_call_response(
  uint32_t service, uint32_t call, const std::string & encoding, const Bytes & data);
Bytes fetch_asset_response(
  uint32_t request, bool ok, const std::string & error, const Bytes & data);

// ── Client → server binary frames ──

struct ClientMessageData
{
  uint32_t channel;
  const uint8_t * data;
  size_t size;
};

struct ServiceCallRequest
{
  uint32_t service;
  uint32_t call;
  std::string encoding;
  const uint8_t * data;
  size_t size;
};

/// A client binary frame, or why it is not one.
using ClientBinary = std::variant<ClientMessageData, ServiceCallRequest, std::string>;
ClientBinary parse_client_binary(const uint8_t * data, size_t size);

}  // namespace miti::protocol
