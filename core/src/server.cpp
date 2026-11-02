#include <chrono>
#include <iostream>
#include <stdexcept>

#include "assets.hpp"
#include "protocol.hpp"
#include "server_impl.hpp"

#ifndef MITI_VERSION
#define MITI_VERSION "0.0.0"
#endif

namespace miti
{

using json = nlohmann::json;

namespace detail
{

ServerImpl::ServerImpl(ServerOptions options_)
: options(std::move(options_))
{
}

void ServerImpl::start()
{
  const tcp::endpoint endpoint{net::ip::make_address(options.address), options.port};
  acceptor.open(endpoint.protocol());
  acceptor.set_option(net::socket_base::reuse_address(true));
  acceptor.bind(endpoint);
  acceptor.listen(net::socket_base::max_listen_connections);
  port = acceptor.local_endpoint().port();
  session_id = std::to_string(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  accept();
  thread = std::thread([this] {
        // A callback that throws must not take the server down with it.
        for (;;) {
          try {
            ioc.run();
            return;
          } catch (const std::exception & exception) {
            std::cerr << "libmiti: a callback threw: " << exception.what() << "\n";
          }
        }
      });
}

void ServerImpl::stop()
{
  if (!thread.joinable()) return;
  if (thread.get_id() == std::this_thread::get_id()) {
    throw std::logic_error("libmiti: stop the server from another thread than its callbacks");
  }
  post([this] {
      beast::error_code ignored;
      acceptor.close(ignored);
      for (const auto & [id, session] : sessions) session->close();
      ioc.stop();
    });
  thread.join();
}

void ServerImpl::accept()
{
  acceptor.async_accept([this](beast::error_code error, tcp::socket socket) {
      if (error) {
        if (error != net::error::operation_aborted) accept();
        return;
      }
      std::make_shared<Session>(*this, std::move(socket), next_session++)->start();
      accept();
    });
}

std::shared_ptr<Channel> ServerImpl::make_channel(ChannelSpec spec)
{
  return std::shared_ptr<Channel>(new Channel(next_channel++, std::move(spec), weak_from_this()));
}

std::vector<std::string> ServerImpl::capabilities() const
{
  std::vector<std::string> list;
  if (options.client_publish.on_message) list.push_back("clientPublish");
  if (options.services) list.push_back("services");
  if (options.get_parameters) list.push_back("parameters");
  if (assets::enabled(options.assets)) list.push_back("assets");
  if (options.time) list.push_back("time");
  return list;
}

std::string ServerImpl::server_info() const
{
  json metadata = options.metadata;
  metadata["miti.version"] = MITI_VERSION;
  json info{
    {"op", "serverInfo"},
    {"name", options.name},
    {"capabilities", capabilities()},
    {"metadata", metadata},
    {"sessionId", session_id},
  };
  if (options.client_publish.on_message) info["supportedEncodings"] = {"cdr"};
  return info.dump();
}

std::string ServerImpl::advertise_all() const
{
  json list = json::array();
  for (const auto & [id, channel] : channels) list.push_back(protocol::channel(*channel));
  return json{{"op", "advertise"}, {"channels", list}}.dump();
}

std::string ServerImpl::advertise_services() const
{
  json list = json::array();
  for (const auto & [id, service] : services) list.push_back(protocol::service(id, service.spec));
  return json{{"op", "advertiseServices"}, {"services", list}}.dump();
}

void ServerImpl::opened(const std::shared_ptr<Session> & session)
{
  sessions[session->id()] = session;
  session->send_text(server_info());
  if (!channels.empty()) session->send_text(advertise_all());
  if (options.services && !services.empty()) session->send_text(advertise_services());
}

void ServerImpl::closed(Session & session)
{
  sessions.erase(session.id());
}

std::string ServerImpl::subscribe(Session & session, uint32_t channel, uint32_t subscription)
{
  const auto found = channels.find(channel);
  if (found == channels.end()) return "Unknown channel " + std::to_string(channel);
  auto & list = subscribers[channel];
  list[session.id()] = subscription;
  if (++found->second->subscribers_ == 1 && options.on_subscribe) {
    options.on_subscribe(*found->second);
  }
  return {};
}

void ServerImpl::unsubscribe(Session & session, uint32_t channel)
{
  const auto list = subscribers.find(channel);
  if (list == subscribers.end() || !list->second.erase(session.id())) return;
  if (list->second.empty()) subscribers.erase(list);
  const auto found = channels.find(channel);
  if (found != channels.end() && --found->second->subscribers_ == 0 && options.on_unsubscribe) {
    options.on_unsubscribe(*found->second);
  }
}

void ServerImpl::remove_channel(uint32_t id)
{
  const auto found = channels.find(id);
  if (found == channels.end()) return;
  // Subscriptions to it end; they need no unsubscribe from the client.
  const auto list = subscribers.find(id);
  if (list != subscribers.end()) {
    for (const auto & [session_id, subscription] : list->second) {
      if (auto target = session(session_id)) target->channel_removed(id);
    }
    subscribers.erase(list);
    found->second->subscribers_ = 0;
    if (options.on_unsubscribe) options.on_unsubscribe(*found->second);
  }
  channels.erase(found);
  broadcast_text(json{{"op", "unadvertise"}, {"channelIds", {id}}}.dump());
}

void ServerImpl::dispatch(
  uint32_t channel, const std::shared_ptr<const Bytes> & message, Timestamp time)
{
  const auto list = subscribers.find(channel);
  if (list == subscribers.end()) return;
  for (const auto & [session_id, subscription] : list->second) {
    if (auto target = session(session_id)) target->send_message(subscription, message, time);
  }
}

void ServerImpl::call_service(
  Session & session, uint32_t service, uint32_t call, const std::string & encoding, Bytes request)
{
  const auto fail = [&](const std::string & message) {
      session.send_text(json{
          {"op", "serviceCallFailure"}, {"serviceId", service}, {"callId", call},
          {"message", message}}.dump());
    };
  const auto found = services.find(service);
  if (!options.services || found == services.end()) {
    fail("Unknown service " + std::to_string(service));
    return;
  }
  const auto & spec = found->second.spec;
  if (encoding != spec.request.encoding) {
    fail("Send " + spec.name + " requests as " + spec.request.encoding + ", not " + encoding);
    return;
  }
  auto state = std::make_shared<ServiceResponder::State>();
  state->server = weak_from_this();
  state->session = session.id();
  state->service = service;
  state->call = call;
  state->encoding = spec.response.encoding;
  found->second.handler(request, ServiceResponder(std::move(state)));
}

void ServerImpl::broadcast_text(const std::string & text)
{
  for (const auto & [id, session] : sessions) session->send_text(text);
}

std::shared_ptr<Session> ServerImpl::session(uint64_t id) const
{
  const auto found = sessions.find(id);
  return found == sessions.end() ? nullptr : found->second;
}

}  // namespace detail

// ── Channel ──

void Channel::log(std::shared_ptr<const Bytes> message, Timestamp receive_time) const
{
  if (!has_subscribers()) return;
  if (auto server = server_.lock()) {
    server->post([impl = server.get(), id = id_, message = std::move(message), receive_time] {
        impl->dispatch(id, message, receive_time);
      });
  }
}

void Channel::log(Bytes message, Timestamp receive_time) const
{
  if (!has_subscribers()) return;
  log(std::make_shared<const Bytes>(std::move(message)), receive_time);
}

// ── ServiceResponder ──

void ServiceResponder::respond(Bytes response) const
{
  state_->answer(std::move(response));
}

void ServiceResponder::fail(std::string message) const
{
  state_->answer(std::move(message));
}

void ServiceResponder::State::answer(std::variant<Bytes, std::string> result)
{
  if (answered.exchange(true)) return;
  auto impl = server.lock();
  if (!impl) return;
  std::string text;
  Bytes binary;
  if (auto * response = std::get_if<Bytes>(&result)) {
    binary = protocol::service_call_response(service, call, encoding, *response);
  } else {
    text = json{
      {"op", "serviceCallFailure"}, {"serviceId", service}, {"callId", call},
      {"message", std::get<std::string>(result)}}.dump();
  }
  impl->post([impl = impl.get(), session = session, text = std::move(text),
    binary = std::move(binary)]() mutable {
      auto target = impl->session(session);
      if (!target) return;
      if (text.empty()) {
        target->send_binary(std::move(binary));
      } else {
        target->send_text(std::move(text));
      }
    });
}

ServiceResponder::State::~State()
{
  if (!answered) answer(std::string("No response"));
}

// ── Server ──

std::unique_ptr<Server> Server::create(ServerOptions options)
{
  auto impl = std::make_shared<detail::ServerImpl>(std::move(options));
  impl->start();
  return std::unique_ptr<Server>(new Server(std::move(impl)));
}

Server::Server(std::shared_ptr<detail::ServerImpl> impl)
: impl_(std::move(impl)) {}

Server::~Server()
{
  stop();
}

uint16_t Server::port() const
{
  return impl_->port;
}

std::shared_ptr<Channel> Server::add_channel(ChannelSpec spec)
{
  auto channel = impl_->make_channel(std::move(spec));
  impl_->post([impl = impl_.get(), channel] {
      impl->channels[channel->id()] = channel;
      json list = json::array({protocol::channel(*channel)});
      impl->broadcast_text(json{{"op", "advertise"}, {"channels", list}}.dump());
    });
  return channel;
}

void Server::remove_channel(const Channel & channel)
{
  impl_->post([impl = impl_.get(), id = channel.id()] {impl->remove_channel(id);});
}

uint32_t Server::add_service(ServiceSpec spec, ServiceHandler handler)
{
  if (!impl_->options.services) {
    throw std::logic_error("libmiti: set ServerOptions::services to add services");
  }
  const uint32_t id = impl_->next_service++;
  impl_->post([impl = impl_.get(), id, spec = std::move(spec), handler = std::move(handler)] {
      impl->services[id] = detail::ServerImpl::Service{spec, handler};
      json list = json::array({protocol::service(id, spec)});
      impl->broadcast_text(json{{"op", "advertiseServices"}, {"services", list}}.dump());
    });
  return id;
}

void Server::remove_service(uint32_t id)
{
  impl_->post([impl = impl_.get(), id] {
      if (!impl->services.erase(id)) return;
      impl->broadcast_text(json{{"op", "unadvertiseServices"}, {"serviceIds", {id}}}.dump());
    });
}

void Server::broadcast_time(Timestamp time)
{
  if (!impl_->options.time) {
    throw std::logic_error("libmiti: set ServerOptions::time to broadcast time");
  }
  impl_->post([impl = impl_.get(), time] {
      for (const auto & [id, session] : impl->sessions) session->send_binary(protocol::time(time));
    });
}

void Server::stop()
{
  impl_->stop();
}

}  // namespace miti
