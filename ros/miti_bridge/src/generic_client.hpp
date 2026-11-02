// A service client for a type known only at run time, with CDR requests and responses.
//
// Humble's rclcpp has no GenericClient (Jazzy's does), so this one is built the way
// prototypes/generic_service_client showed works on both: an rcl client initialized with the
// type support looked up by symbol name, requests deserialized into memory laid out by the
// introspection type support. As an rclcpp::ClientBase added to the node, it gets its responses
// from the executor rather than by polling.

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include <rclcpp/rclcpp.hpp>

namespace miti_bridge
{

class GenericClient : public rclcpp::ClientBase
{
public:
  /// The response's CDR bytes, or why there is none.
  using Result = std::variant<std::vector<uint8_t>, std::string>;
  using Callback = std::function<void(Result)>;

  /// Creates the client and adds it to the node, so its responses arrive on the node's executor.
  static std::shared_ptr<GenericClient> create(
    rclcpp::Node & node, const std::string & service, const std::string & type);

  /// Sends a CDR request. `done` is called once: with the response, or with a failure.
  void call(const std::vector<uint8_t> & request, Callback done,
    std::chrono::steady_clock::duration timeout);
  /// Fails the calls whose time ran out.
  void expire(std::chrono::steady_clock::time_point now);

  std::shared_ptr<void> create_response() override;
  std::shared_ptr<rmw_request_id_t> create_request_header() override;
  void handle_response(
    std::shared_ptr<rmw_request_id_t> header, std::shared_ptr<void> response) override;

private:
  GenericClient(rclcpp::Node & node, const std::string & service, const std::string & type);

  /// A message of the request or response type, laid out by its introspection type support.
  std::shared_ptr<void> allocate(const rosidl_message_type_support_t * layout) const;

  std::string service_;
  const rosidl_message_type_support_t * request_cpp_ = nullptr;
  const rosidl_message_type_support_t * response_cpp_ = nullptr;
  const rosidl_message_type_support_t * request_layout_ = nullptr;
  const rosidl_message_type_support_t * response_layout_ = nullptr;

  struct Pending
  {
    Callback done;
    std::chrono::steady_clock::time_point deadline;
  };
  std::mutex mutex_;
  std::map<int64_t, Pending> pending_;  // by request sequence number
};

}  // namespace miti_bridge
