#include <chrono>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "assets.hpp"
#include "protocol.hpp"
#include "server_impl.hpp"

namespace miti::detail
{

using json = nlohmann::json;
using protocol::StatusLevel;

namespace
{

#ifdef TCP_NOTSENT_LOWAT
/// The most a connection's socket holds unsent. A little keeps the link busy; more would hold
/// frames where a newer one can no longer replace them.
constexpr int NOTSENT_LOWAT = 128 * 1024;
#endif

/// `pkg/Type` → `pkg/msg/Type`, to compare type names written either way.
std::string full_type(const std::string & type)
{
  const auto first = type.find('/');
  if (first == std::string::npos || type.find('/', first + 1) != std::string::npos) return type;
  return type.substr(0, first) + "/msg" + type.substr(first);
}

}  // namespace

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
  auto & socket = beast::get_lowest_layer(ws_).socket();
  beast::error_code ignored;
  // A small message, such as TF, goes out at once instead of waiting behind a frame's last bytes.
  socket.set_option(tcp::no_delay(true), ignored);
#ifdef TCP_NOTSENT_LOWAT
  // Frames then wait in the scheduler rather than in the kernel's send buffer, which grows to
  // megabytes on a slow link: there they could only arrive late.
  ::setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &NOTSENT_LOWAT,
    sizeof(NOTSENT_LOWAT));
#else
  // Windows has no TCP_NOTSENT_LOWAT. There the send buffer follows the connection's ideal send
  // backlog, about one bandwidth-delay product, unless SO_SNDBUF is set: it is left as it is.
#endif
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
  } else if (op == "miti.getSchema") {
    get_schema(message);
  } else if (op == "miti.subscribeStats") {
    subscribe_stats(message);
  } else if (op == "miti.unsubscribeStats") {
    ++stats_generation_;
    stats_timer_.reset();
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
    const auto found = server_.channels.find(channel);
    if (found == server_.channels.end()) {
      error("Unknown channel " + std::to_string(channel));
      continue;
    }
    channel_of_[subscription] = channel;
    subscription_of_[channel] = subscription;
    scheduler_.add(subscription, default_options(found->second->spec().message.schema_name));
    states_[subscription] = {};
    server_.subscribe(*this, channel, subscription);
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
    states_.erase(subscription);
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
  // Read on another thread, so a slow disk does not stall every connection meanwhile.
  net::post(server_.assets_pool, [impl = &server_, session = id_, request, uri] {
      const auto result = assets::fetch(impl->options.assets, uri);
      auto frame = protocol::fetch_asset_response(request, result.ok, result.error, result.data);
      impl->post([impl, session, frame = std::move(frame)]() mutable {
          if (auto target = impl->session(session)) target->send_binary(std::move(frame));
        });
    });
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
  std::optional<transforms::Transform> transform = states_[subscription].transform;
  const auto & channel = server_.channels.at(channel_of_.at(subscription))->spec().message;
  if (message.contains("transform")) {
    transform.reset();
    if (!message["transform"].is_null()) {
      auto parsed = transforms::parse(message["transform"]);
      if (const auto * failure = std::get_if<std::string>(&parsed)) {
        error(*failure);
        return;
      }
      const auto & wanted = std::get<transforms::Transform>(parsed);
      const std::string input = transforms::input_type(wanted.kind);
      if (full_type(channel.schema_name) != input) {
        error(message["transform"].value("kind", "") + " applies to " + input + " topics, not " +
          channel.schema_name);
        return;
      }
      transform = wanted;
    }
  }
  *options = updated;

  auto & state = states_[subscription];
  const auto key = [](const std::optional<transforms::Transform> & t) {
      return t ? t->key() : std::string();
    };
  if (key(state.transform) == key(transform)) return;
  // Messages waiting in the old format go, and so do transformed ones still on their way.
  const auto format = [&channel](const std::optional<transforms::Transform> & t) {
      const auto output = t ? transforms::output_definition(*t) : std::nullopt;
      return output ? *output : channel;
    };
  const auto before = format(state.transform);
  const auto after = format(transform);
  state.transform = transform;
  ++state.generation;
  state.warned = false;
  scheduler_.clear(subscription);
  if (before.schema_name != after.schema_name || before.schema != after.schema) {
    send_text(json{
        {"op", "miti.subscriptionFormat"}, {"subscriptionId", subscription},
        {"encoding", after.encoding}, {"schemaName", after.schema_name},
        {"schemaEncoding", after.schema_encoding}, {"schema", after.schema}}.dump());
  }
}

void Session::subscribe_stats(const json & message)
{
  const auto interval = message.value("intervalMs", 1000);
  if (interval < 250) {
    error("intervalMs is at least 250");
    return;
  }
  stats_interval_ = std::chrono::milliseconds(interval);
  stats_since_ = Scheduler::Clock::now();
  scheduler_.report(stats_since_);  // counts start now
  stats_timer_.emplace(ws_.get_executor());
  schedule_stats(++stats_generation_);
}

void Session::schedule_stats(uint64_t generation)
{
  stats_timer_->expires_after(stats_interval_);
  stats_timer_->async_wait([self = shared_from_this(), generation](beast::error_code error) {
      // Stopped, or subscribed again with another interval since.
      if (error || self->ended_ || !self->stats_timer_ || self->stats_generation_ != generation) {
        return;
      }
      self->send_stats();
      self->schedule_stats(generation);
    });
}

void Session::send_stats()
{
  const auto now = Scheduler::Clock::now();
  const double seconds = std::chrono::duration<double>(now - stats_since_).count();
  stats_since_ = now;
  json subscriptions = json::array();
  for (const auto & report : scheduler_.report(now)) {
    subscriptions.push_back({
        {"subscriptionId", report.subscription},
        {"bytesPerSecond", seconds > 0 ? report.sent_bytes / seconds : 0},
        {"sent", report.sent},
        {"dropped", report.dropped},
        {"oldestQueuedMs", report.oldest_queued_ms}});
  }
  const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  send_text(json{
      {"op", "miti.stats"}, {"timestamp", unix_ms},
      {"bufferedBytes", scheduler_.queued_bytes() + (writing_ ? writing_frame_.size() : 0)},
      {"subscriptions", subscriptions}}.dump());
}

void Session::get_schema(const json & message)
{
  if (!server_.options.get_schema) {
    error("This server does not look up schemas (no miti.schemas capability)");
    return;
  }
  const auto type = message.at("type").get<std::string>();
  json reply{{"op", "miti.schema"}, {"type", type}};
  if (message.contains("id")) reply["id"] = message["id"];
  if (const auto definition = server_.options.get_schema(type)) {
    reply["schemaName"] = definition->schema_name;
    reply["schemaEncoding"] = definition->schema_encoding;
    reply["schema"] = definition->schema;
  } else {
    reply["error"] = "Unknown type " + type;
  }
  send_text(reply.dump());
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
  const auto now = Scheduler::Clock::now();
  const auto state = states_.find(subscription);
  if (state != states_.end() && state->second.transform) {
    // maxRate first: a message that would be dropped is never transformed.
    if (!scheduler_.admit(subscription, now)) return;
    server_.transform(channel_of_.at(subscription), *state->second.transform, message, time,
      {id_, subscription, state->second.generation});
    return;
  }
  const auto header = protocol::message_data_header(subscription, time);
  Frame frame{true, Bytes(header.begin(), header.end()), std::move(message)};
  if (scheduler_.push_message(subscription, std::move(frame), now)) pump();
}

void Session::deliver(
  uint32_t subscription, uint32_t generation, std::shared_ptr<const Bytes> output,
  const std::string & warning, Timestamp time)
{
  const auto state = states_.find(subscription);
  if (ended_ || state == states_.end() || state->second.generation != generation) return;
  if (!output) {
    scheduler_.count_dropped(subscription);
    if (!state->second.warned) {
      state->second.warned = true;
      send_text(protocol::status(StatusLevel::warning,
        "Subscription " + std::to_string(subscription) + ": " + warning));
    }
    return;
  }
  const auto header = protocol::message_data_header(subscription, time);
  scheduler_.enqueue(subscription, Frame{true, Bytes(header.begin(), header.end()), std::move(output)},
    Scheduler::Clock::now());
  pump();
}

void Session::channel_removed(uint32_t channel)
{
  const auto found = subscription_of_.find(channel);
  if (found == subscription_of_.end()) return;
  scheduler_.remove(found->second);
  states_.erase(found->second);
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
  stats_timer_.reset();
  states_.clear();
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
