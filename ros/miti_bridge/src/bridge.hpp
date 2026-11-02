// Serves a ROS 2 graph to MITI through libmiti: each topic is a channel, each service a service;
// parameters, packages' files and the clock go through as well.

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <miti/server.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rosgraph_msgs/msg/clock.hpp>

#include "definitions.hpp"
#include "generic_client.hpp"

namespace miti_bridge
{

class Bridge
{
public:
  /// Declares the node's parameters and starts serving; throws when the port cannot be bound.
  explicit Bridge(rclcpp::Node::SharedPtr node);
  ~Bridge();

  uint16_t port() const {return server_->port();}

private:
  // The graph timer, on the executor's thread.
  void update_graph();
  void update_topics();
  void update_services();
  void expire();

  // libmiti callbacks, on the server's thread.
  void subscribe(const miti::Channel & channel);
  void unsubscribe(const miti::Channel & channel);
  void client_advertise(const miti::ClientChannel & channel);
  void client_publish(const miti::ClientChannel & channel, const uint8_t * data, size_t size);
  void client_unadvertise(const miti::ClientChannel & channel);
  void get_parameters(const std::vector<std::string> & names,
    std::function<void(std::vector<miti::Parameter>)> reply);
  void call_service(const std::string & name, const miti::Bytes & request,
    miti::ServiceResponder responder);

  rclcpp::Node::SharedPtr node_;
  struct Settings
  {
    std::vector<std::regex> topics, services, parameters, client_topics;
    std::set<std::string> capabilities;
    size_t max_qos_depth;
  } settings_;
  rclcpp::TimerBase::SharedPtr graph_timer_;
  rclcpp::Subscription<rosgraph_msgs::msg::Clock>::SharedPtr clock_;

  // Guards what follows: the graph timer, the server's thread and ROS callbacks all use it.
  std::mutex mutex_;
  Definitions definitions_;
  std::set<std::string> unusable_types_;  // warned about once
  struct Topic
  {
    std::string name, type;
    std::shared_ptr<miti::Channel> channel;
    rclcpp::GenericSubscription::SharedPtr subscription;
  };
  std::map<std::pair<std::string, std::string>, Topic> topics_;  // by (name, type)
  std::map<uint32_t, std::pair<std::string, std::string>> topic_of_channel_;
  struct Service
  {
    uint32_t id;
    std::string type;
    std::shared_ptr<GenericClient> client;
  };
  std::map<std::string, Service> services_;
  std::map<std::pair<uint64_t, uint32_t>, rclcpp::GenericPublisher::SharedPtr> publishers_;
  std::map<std::string, std::shared_ptr<rclcpp::AsyncParametersClient>> parameter_clients_;
  struct ParameterRequest;
  std::vector<std::shared_ptr<ParameterRequest>> parameter_requests_;

  // Last, so it stops first: its callbacks use everything above.
  std::unique_ptr<miti::Server> server_;
};

}  // namespace miti_bridge
