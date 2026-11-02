// The server's inside: one I/O thread runs everything below, so none of it needs a lock. Public
// methods called from other threads post their work to that thread.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "miti/server.hpp"
#include "scheduler.hpp"

namespace miti::detail
{

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

struct ServerImpl;

/// One client connection.
class Session : public std::enable_shared_from_this<Session>
{
public:
  Session(ServerImpl & server, tcp::socket socket, uint64_t id);

  /// Reads the HTTP upgrade and accepts it if it offers miti.bridge.v1; refuses it otherwise.
  void start();
  uint64_t id() const {return id_;}

  void send_text(std::string text);
  void send_binary(Bytes frame);
  void send_message(uint32_t subscription, std::shared_ptr<const Bytes> message, Timestamp time);
  /// The server removed a channel this session may be subscribed to.
  void channel_removed(uint32_t channel);
  void close();

private:
  void refuse(std::string reason);
  void accepted();
  void read();
  void on_text(const std::string & text);
  void on_binary(const uint8_t * data, size_t size);
  void handle(const std::string & op, const nlohmann::json & message);
  void subscribe(const nlohmann::json & message);
  void unsubscribe(const nlohmann::json & message);
  void client_advertise(const nlohmann::json & message);
  void client_unadvertise(const nlohmann::json & message);
  void get_parameters(const nlohmann::json & message);
  void fetch_asset(const nlohmann::json & message);
  void subscription_options(const nlohmann::json & message);
  void error(const std::string & message);
  void pump();
  void ended();

  ServerImpl & server_;
  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> request_;
  uint64_t id_;
  bool open_ = false;
  bool writing_ = false;
  bool ended_ = false;
  Scheduler scheduler_;
  Frame writing_frame_;
  std::map<uint32_t, uint32_t> channel_of_;       // subscription → channel
  std::map<uint32_t, uint32_t> subscription_of_;  // channel → subscription
  std::map<uint32_t, ClientChannel> client_channels_;
};

struct ServerImpl : std::enable_shared_from_this<ServerImpl>
{
  explicit ServerImpl(ServerOptions options);

  void start();
  void stop();

  template<typename F>
  void post(F && work) {net::post(ioc, std::forward<F>(work));}

  std::shared_ptr<Channel> make_channel(ChannelSpec spec);
  std::vector<std::string> capabilities() const;
  std::string server_info() const;
  std::string advertise_all() const;
  std::string advertise_services() const;

  // ── I/O thread only ──
  void accept();
  void opened(const std::shared_ptr<Session> & session);
  void closed(Session & session);
  /// An error message, or empty when subscribed.
  std::string subscribe(Session & session, uint32_t channel, uint32_t subscription);
  void unsubscribe(Session & session, uint32_t channel);
  void remove_channel(uint32_t id);
  void dispatch(uint32_t channel, const std::shared_ptr<const Bytes> & message, Timestamp time);
  void call_service(Session & session, uint32_t service, uint32_t call, const std::string & encoding,
    Bytes request);
  void broadcast_text(const std::string & text);
  std::shared_ptr<Session> session(uint64_t id) const;

  // Declared first so it is destroyed last: handlers it still holds reference the rest.
  net::io_context ioc{1};
  const ServerOptions options;
  tcp::acceptor acceptor{ioc};
  std::thread thread;
  uint16_t port = 0;
  std::string session_id;
  std::atomic<uint32_t> next_channel{1};
  std::atomic<uint32_t> next_service{1};

  uint64_t next_session = 1;
  std::map<uint32_t, std::shared_ptr<Channel>> channels;
  /// channel → session → subscription
  std::map<uint32_t, std::map<uint64_t, uint32_t>> subscribers;
  struct Service
  {
    ServiceSpec spec;
    ServiceHandler handler;
  };
  std::map<uint32_t, Service> services;
  std::map<uint64_t, std::shared_ptr<Session>> sessions;
};

}  // namespace miti::detail

namespace miti
{

/// Shared by the copies of one ServiceResponder.
struct ServiceResponder::State
{
  std::weak_ptr<detail::ServerImpl> server;
  uint64_t session;
  uint32_t service;
  uint32_t call;
  std::string encoding;
  std::atomic<bool> answered{false};

  /// The response bytes, or the failure message.
  void answer(std::variant<Bytes, std::string> result);
  ~State();
};

}  // namespace miti
