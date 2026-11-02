#include <chrono>

#include "assets.hpp"
#include "protocol.hpp"
#include "server_impl.hpp"

namespace miti::detail
{

using json = nlohmann::json;
using protocol::StatusLevel;

Session::Session(ServerImpl & server, tcp::socket socket, uint64_t id)
: server_(server), ws_(std::move(socket)), id_(id), scheduler_(server.options.queue_limit)
{
}

void Session::start()
{
  beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
  http::async_read(
    ws_.next_layer(), buffer_, request_,
    [self = shared_from_this()](beast::error_code error, size_t) {
      if (error) return;
      if (!websocket::is_upgrade(self->request_)) {
        self->refuse("This is a MITI server: connect with a WebSocket");
        return;
      }
      const auto offered = self->request_[http::field::sec_websocket_protocol];
      if (!protocol::offers(std::string_view(offered.data(), offered.size()), protocol::SUBPROTOCOL)) {
        self->refuse("Offer the WebSocket subprotocol miti.bridge.v1");
        return;
      }
      self->ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type & response) {
          response.set(http::field::sec_websocket_protocol, protocol::SUBPROTOCOL);
          response.set(http::field::server, "libmiti");
        }));
      self->ws_.async_accept(self->request_, [self](beast::error_code accept_error) {
          if (!accept_error) self->accepted();
        });
    });
}

void Session::refuse(std::string reason)
{
  auto response = std::make_shared<http::response<http::string_body>>(
    http::status::bad_request, request_.version());
  response->set(http::field::server, "libmiti");
  response->set(http::field::content_type, "text/plain");
  response->body() = std::move(reason);
  response->prepare_payload();
  http::async_write(
    ws_.next_layer(), *response,
    [self = shared_from_this(), response](beast::error_code, size_t) {
      beast::error_code ignored;
      beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_both, ignored);
    });
}

void Session::accepted()
{
  beast::get_lowest_layer(ws_).expires_never();
  // Pings keep an idle connection, such as a viewer that only receives, from timing out.
  websocket::stream_base::timeout timeout{};
  timeout.handshake_timeout = std::chrono::seconds(30);
  timeout.idle_timeout = std::chrono::seconds(60);
  timeout.keep_alive_pings = true;
  ws_.set_option(timeout);
  open_ = true;
  server_.opened(shared_from_this());
  read();
}

void Session::read()
{
  ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code error, size_t) {
      if (error) {
        self->ended();
        return;
      }
      const auto data = self->buffer_.cdata();
      if (self->ws_.got_text()) {
        self->on_text(std::string(static_cast<const char *>(data.data()), data.size()));
      } else {
        self->on_binary(static_cast<const uint8_t *>(data.data()), data.size());
      }
      self->buffer_.consume(self->buffer_.size());
      if (!self->ended_) self->read();
    });
}

void Session::on_text(const std::string & text)
{
  const auto message = json::parse(text, nullptr, false);
  if (message.is_discarded() || !message.is_object() || !message.contains("op") ||
    !message["op"].is_string())
  {
    error("Expected a JSON object with an op");
    return;
  }
  const auto op = message["op"].get<std::string>();
  try {
    handle(op, message);
  } catch (const json::exception & exception) {
    error("Invalid " + op + ": " + exception.what());
  }
}

void Session::handle(const std::string & op, const json & message)
{
  if (op == "subscribe") {
    subscribe(message);
  } else if (op == "unsubscribe") {
    unsubscribe(message);
  } else if (op == "advertise") {
    client_advertise(message);
  } else if (op == "unadvertise") {
    client_unadvertise(message);
  } else if (op == "getParameters") {
    get_parameters(message);
  } else if (op == "fetchAsset") {
    fetch_asset(message);
  } else if (op == "miti.subscriptionOptions") {
    subscription_options(message);
  } else {
    send_text(protocol::status(StatusLevel::warning, "Unknown operation " + op));
  }
}

void Session::subscribe(const json & message)
{
  for (const auto & entry : message.at("subscriptions")) {
    const auto subscription = entry.at("id").get<uint32_t>();
    const auto channel = entry.at("channelId").get<uint32_t>();
    if (channel_of_.count(subscription)) {
      error("Subscription id " + std::to_string(subscription) + " is already used");
      continue;
    }
    if (subscription_of_.count(channel)) {
      error("Already subscribed to channel " + std::to_string(channel));
      continue;
    }
    const auto failure = server_.subscribe(*this, channel, subscription);
    if (!failure.empty()) {
      error(failure);
      continue;
    }
    channel_of_[subscription] = channel;
    subscription_of_[channel] = subscription;
    scheduler_.add(
      subscription,
      default_options(server_.channels.at(channel)->spec().message.schema_name));
  }
}

void Session::unsubscribe(const json & message)
{
  for (const auto & id : message.at("subscriptionIds")) {
    const auto subscription = id.get<uint32_t>();
    const auto found = channel_of_.find(subscription);
    if (found == channel_of_.end()) {
      error("Unknown subscription " + std::to_string(subscription));
      continue;
    }
    const uint32_t channel = found->second;
    scheduler_.remove(subscription);
    channel_of_.erase(found);
    subscription_of_.erase(channel);
    server_.unsubscribe(*this, channel);
  }
}

void Session::client_advertise(const json & message)
{
  const auto & handlers = server_.options.client_publish;
  if (!handlers.on_message) {
    error("This server does not accept published messages (no clientPublish capability)");
    return;
  }
  for (const auto & entry : message.at("channels")) {
    ClientChannel channel{
      id_,
      entry.at("id").get<uint32_t>(),
      entry.at("topic").get<std::string>(),
      {entry.at("encoding").get<std::string>(), entry.at("schemaName").get<std::string>(),
        entry.value("schemaEncoding", std::string()), entry.value("schema", std::string())}};
    if (channel.message.encoding != "cdr") {
      error("Unsupported encoding " + channel.message.encoding + " for " + channel.topic +
        ": publish cdr");
      continue;
    }
    if (client_channels_.count(channel.id)) {
      error("Client channel " + std::to_string(channel.id) + " is already advertised");
      continue;
    }
    client_channels_[channel.id] = channel;
    if (handlers.on_advertise) handlers.on_advertise(channel);
  }
}

void Session::client_unadvertise(const json & message)
{
  for (const auto & id : message.at("channelIds")) {
    const auto found = client_channels_.find(id.get<uint32_t>());
    if (found == client_channels_.end()) {
      error("Unknown client channel " + id.dump());
      continue;
    }
    const auto channel = found->second;
    client_channels_.erase(found);
    if (server_.options.client_publish.on_unadvertise) {
      server_.options.client_publish.on_unadvertise(channel);
    }
  }
}

void Session::get_parameters(const json & message)
{
  if (!server_.options.get_parameters) {
    error("This server does not offer parameters (no parameters capability)");
    return;
  }
  const auto names = message.at("parameterNames").get<std::vector<std::string>>();
  std::optional<std::string> request;
  if (message.contains("id")) request = message["id"].get<std::string>();
  auto reply = [server = server_.weak_from_this(), session = id_, request](
    std::vector<Parameter> parameters) {
      auto impl = server.lock();
      if (!impl) return;
      json values = json::array();
      for (const auto & parameter : parameters) values.push_back(protocol::parameter(parameter));
      json response{{"op", "parameterValues"}, {"parameters", values}};
      if (request) response["id"] = *request;
      impl->post([impl = impl.get(), session, text = response.dump()]() mutable {
          if (auto target = impl->session(session)) target->send_text(std::move(text));
        });
    };
  server_.options.get_parameters(names, std::move(reply));
}

void Session::fetch_asset(const json & message)
{
  const auto uri = message.at("uri").get<std::string>();
  const auto request = message.at("requestId").get<uint32_t>();
  if (!assets::enabled(server_.options.assets)) {
    error("This server does not serve assets (no assets capability)");
    return;
  }
  // Files are read on this thread: assets are meshes of a few megabytes, fetched once each.
  const auto result = assets::fetch(server_.options.assets, uri);
  send_binary(protocol::fetch_asset_response(request, result.ok, result.error, result.data));
}

void Session::subscription_options(const json & message)
{
  const auto subscription = message.at("subscriptionId").get<uint32_t>();
  auto * options = scheduler_.options(subscription);
  if (!options) {
    error("Unknown subscription " + std::to_string(subscription));
    return;
  }
  auto updated = *options;
  if (message.contains("maxRate")) {
    const auto & rate = message["maxRate"];
    if (rate.is_null()) {
      updated.max_rate = 0;
    } else if (rate.is_number() && rate.get<double>() > 0) {
      updated.max_rate = rate.get<double>();
    } else {
      error("maxRate must be a number greater than 0, or null");
      return;
    }
  }
  if (message.contains("delivery")) {
    const auto delivery = message["delivery"].get<std::string>();
    if (delivery != "all" && delivery != "latest") {
      error("delivery must be all or latest");
      return;
    }
    updated.delivery = delivery == "all" ? Delivery::all : Delivery::latest;
  }
  if (message.contains("priority")) {
    const auto priority = message["priority"].get<std::string>();
    if (priority == "high") {
      updated.priority = Priority::high;
    } else if (priority == "normal") {
      updated.priority = Priority::normal;
    } else if (priority == "low") {
      updated.priority = Priority::low;
    } else {
      error("priority must be high, normal or low");
      return;
    }
  }
  if (message.contains("transform") && !message["transform"].is_null()) {
    error("This server does not transform messages (no miti.transforms capability)");
    return;
  }
  *options = updated;
}

void Session::on_binary(const uint8_t * data, size_t size)
{
  const auto frame = protocol::parse_client_binary(data, size);
  if (const auto * failure = std::get_if<std::string>(&frame)) {
    error(*failure);
  } else if (const auto * published = std::get_if<protocol::ClientMessageData>(&frame)) {
    const auto channel = client_channels_.find(published->channel);
    if (channel == client_channels_.end()) {
      error("Unknown client channel " + std::to_string(published->channel));
      return;
    }
    server_.options.client_publish.on_message(channel->second, published->data, published->size);
  } else if (const auto * call = std::get_if<protocol::ServiceCallRequest>(&frame)) {
    server_.call_service(
      *this, call->service, call->call, call->encoding, Bytes(call->data, call->data + call->size));
  }
}

void Session::error(const std::string & message)
{
  send_text(protocol::status(StatusLevel::error, message));
}

void Session::send_text(std::string text)
{
  if (ended_) return;
  scheduler_.push_control(Frame{false, Bytes(text.begin(), text.end()), nullptr});
  pump();
}

void Session::send_binary(Bytes frame)
{
  if (ended_) return;
  scheduler_.push_control(Frame{true, std::move(frame), nullptr});
  pump();
}

void Session::send_message(
  uint32_t subscription, std::shared_ptr<const Bytes> message, Timestamp time)
{
  if (ended_) return;
  const auto header = protocol::message_data_header(subscription, time);
  Frame frame{true, Bytes(header.begin(), header.end()), std::move(message)};
  if (scheduler_.push_message(subscription, std::move(frame), Scheduler::Clock::now())) pump();
}

void Session::channel_removed(uint32_t channel)
{
  const auto found = subscription_of_.find(channel);
  if (found == subscription_of_.end()) return;
  scheduler_.remove(found->second);
  channel_of_.erase(found->second);
  subscription_of_.erase(found);
}

void Session::pump()
{
  if (!open_ || writing_ || ended_) return;
  auto frame = scheduler_.next();
  if (!frame) return;
  writing_ = true;
  writing_frame_ = std::move(*frame);
  ws_.binary(writing_frame_.binary);
  auto done = [self = shared_from_this()](beast::error_code error, size_t) {
      self->writing_ = false;
      self->writing_frame_ = Frame{};
      if (error) {
        self->ended();
        return;
      }
      self->pump();
    };
  if (writing_frame_.body) {
    const std::array<net::const_buffer, 2> buffers{
      net::buffer(writing_frame_.head), net::buffer(*writing_frame_.body)};
    ws_.async_write(buffers, std::move(done));
  } else {
    ws_.async_write(net::buffer(writing_frame_.head), std::move(done));
  }
}

void Session::close()
{
  beast::error_code ignored;
  beast::get_lowest_layer(ws_).socket().close(ignored);
}

void Session::ended()
{
  if (ended_) return;
  ended_ = true;
  for (const auto & [channel, subscription] : subscription_of_) server_.unsubscribe(*this, channel);
  subscription_of_.clear();
  channel_of_.clear();
  if (server_.options.client_publish.on_unadvertise) {
    for (const auto & [id, channel] : client_channels_) {
      server_.options.client_publish.on_unadvertise(channel);
    }
  }
  client_channels_.clear();
  if (open_) server_.closed(*this);
}

}  // namespace miti::detail
