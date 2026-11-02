#include "protocol.hpp"

#include <type_traits>

namespace miti::protocol
{

namespace
{

void put_u32(uint8_t * out, uint32_t value)
{
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

void put_u64(uint8_t * out, uint64_t value)
{
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t get_u32(const uint8_t * in)
{
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (8 * i);
  return value;
}

void append_u32(Bytes & out, uint32_t value)
{
  out.resize(out.size() + 4);
  put_u32(out.data() + out.size() - 4, value);
}

void append_string(Bytes & out, const std::string & text)
{
  append_u32(out, static_cast<uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

std::string base64(const std::vector<uint8_t> & bytes)
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t chunk = (bytes[i] << 16) |
      (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
    out += alphabet[(chunk >> 18) & 63];
    out += alphabet[(chunk >> 12) & 63];
    out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
  }
  return out;
}

nlohmann::json definition(const MessageDefinition & message)
{
  return {
    {"encoding", message.encoding},
    {"schemaName", message.schema_name},
    {"schemaEncoding", message.schema_encoding},
    {"schema", message.schema},
  };
}

std::string_view trim(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

}  // namespace

bool offers(std::string_view header, std::string_view protocol)
{
  while (!header.empty()) {
    const auto comma = header.find(',');
    if (trim(header.substr(0, comma)) == protocol) return true;
    if (comma == std::string_view::npos) break;
    header.remove_prefix(comma + 1);
  }
  return false;
}

nlohmann::json channel(const Channel & channel)
{
  const auto & message = channel.spec().message;
  nlohmann::json json{
    {"id", channel.id()},
    {"topic", channel.spec().topic},
    {"encoding", message.encoding},
    {"schemaName", message.schema_name},
    {"schema", message.schema},
  };
  if (!message.schema_encoding.empty()) json["schemaEncoding"] = message.schema_encoding;
  return json;
}

nlohmann::json service(uint32_t id, const ServiceSpec & spec)
{
  return {
    {"id", id},
    {"name", spec.name},
    {"type", spec.type},
    {"request", definition(spec.request)},
    {"response", definition(spec.response)},
  };
}

nlohmann::json parameter(const Parameter & parameter)
{
  nlohmann::json json{{"name", parameter.name}};
  std::visit(
    [&json](const auto & value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
        json["value"] = base64(value);
        json["type"] = "byte_array";
      } else {
        json["value"] = value;
        if constexpr (std::is_same_v<T, double>) json["type"] = "float64";
        if constexpr (std::is_same_v<T, std::vector<double>>) json["type"] = "float64_array";
      }
    },
    parameter.value);
  return json;
}

std::string status(StatusLevel level, const std::string & message)
{
  return nlohmann::json{
    {"op", "status"}, {"level", static_cast<int>(level)}, {"message", message}}.dump();
}

std::array<uint8_t, 13> message_data_header(uint32_t subscription, Timestamp time)
{
  std::array<uint8_t, 13> header{};
  header[0] = 0x01;
  put_u32(header.data() + 1, subscription);
  put_u64(header.data() + 5, time);
  return header;
}

Bytes time(Timestamp time)
{
  Bytes out(9);
  out[0] = 0x02;
  put_u64(out.data() + 1, time);
  return out;
}

Bytes service_call_response(
  uint32_t service, uint32_t call, const std::string & encoding, const Bytes & data)
{
  Bytes out{0x03};
  append_u32(out, service);
  append_u32(out, call);
  append_string(out, encoding);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

Bytes fetch_asset_response(
  uint32_t request, bool ok, const std::string & error, const Bytes & data)
{
  Bytes out{0x04};
  append_u32(out, request);
  out.push_back(ok ? 0 : 1);
  append_string(out, ok ? std::string() : error);
  if (ok) out.insert(out.end(), data.begin(), data.end());
  return out;
}

ClientBinary parse_client_binary(const uint8_t * data, size_t size)
{
  if (size < 1) return std::string("Empty binary frame");
  switch (data[0]) {
    case 0x01:
      if (size < 5) return std::string("Client message data is too short");
      return ClientMessageData{get_u32(data + 1), data + 5, size - 5};
    case 0x02: {
      if (size < 13) return std::string("Service call request is too short");
      const uint32_t length = get_u32(data + 9);
      if (size - 13 < length) return std::string("Service call request encoding is cut off");
      std::string encoding(reinterpret_cast<const char *>(data + 13), length);
      const size_t offset = 13 + length;
      return ServiceCallRequest{
        get_u32(data + 1), get_u32(data + 5), std::move(encoding), data + offset, size - offset};
    }
    default:
      return "Unknown client binary opcode " + std::to_string(data[0]);
  }
}

}  // namespace miti::protocol
