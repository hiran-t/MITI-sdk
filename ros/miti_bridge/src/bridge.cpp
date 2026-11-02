#include "bridge.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include <ament_index_cpp/get_resources.hpp>

namespace miti_bridge
{

namespace
{

constexpr auto GRAPH_PERIOD = std::chrono::seconds(1);
constexpr auto SERVICE_TIMEOUT = std::chrono::seconds(10);
constexpr auto PARAMETER_TIMEOUT = std::chrono::seconds(5);

/// Topics and services with a part that starts with "_" are hidden in ROS.
bool hidden(const std::string & name)
{
  return name.find("/_") != std::string::npos;
}

bool matches(const std::vector<std::regex> & patterns, const std::string & name)
{
  return std::any_of(patterns.begin(), patterns.end(), [&name](const std::regex & pattern) {
             return std::regex_match(name, pattern);
           });
}

std::vector<std::regex> patterns(rclcpp::Node & node, const std::string & parameter)
{
  std::vector<std::regex> compiled;
  for (const auto & text : node.declare_parameter(parameter, std::vector<std::string>{".*"})) {
    compiled.emplace_back(text);
  }
  return compiled;
}

/// "/ns/node.param.name" → {"/ns/node", "param.name"}; node names have no dots.
std::pair<std::string, std::string> split_parameter(const std::string & full)
{
  const auto slash = full.rfind('/');
  const auto dot = full.find('.', slash == std::string::npos ? 0 : slash);
  if (slash == std::string::npos || dot == std::string::npos) return {};
  return {full.substr(0, dot), full.substr(dot + 1)};
}

std::optional<miti::ParameterValue> value(const rclcpp::Parameter & parameter)
{
  switch (parameter.get_type()) {
    case rclcpp::ParameterType::PARAMETER_BOOL: return parameter.as_bool();
    case rclcpp::ParameterType::PARAMETER_INTEGER: return int64_t{parameter.as_int()};
    case rclcpp::ParameterType::PARAMETER_DOUBLE: return parameter.as_double();
    case rclcpp::ParameterType::PARAMETER_STRING: return parameter.as_string();
    case rclcpp::ParameterType::PARAMETER_BYTE_ARRAY: return parameter.as_byte_array();
    case rclcpp::ParameterType::PARAMETER_BOOL_ARRAY: return parameter.as_bool_array();
    case rclcpp::ParameterType::PARAMETER_INTEGER_ARRAY: return parameter.as_integer_array();
    case rclcpp::ParameterType::PARAMETER_DOUBLE_ARRAY: return parameter.as_double_array();
    case rclcpp::ParameterType::PARAMETER_STRING_ARRAY: return parameter.as_string_array();
    default: return std::nullopt;  // not set
  }
}

}  // namespace

/// One getParameters request, answered once every node has replied or the time is up.
struct Bridge::ParameterRequest
{
  std::mutex mutex;
  std::vector<miti::Parameter> found;
  size_t waiting = 0;
  bool answered = false;
  std::function<void(std::vector<miti::Parameter>)> reply;
  std::chrono::steady_clock::time_point deadline;

  /// One node's reply arrived, or will not.
  void done(std::vector<miti::Parameter> parameters)
  {
    std::lock_guard<std::mutex> lock(mutex);
    found.insert(found.end(), parameters.begin(), parameters.end());
    if (--waiting == 0) answer();
  }

  /// Under `mutex`.
  void answer()
  {
    if (answered) return;
    answered = true;
    reply(found);
  }
};

Bridge::Bridge(rclcpp::Node::SharedPtr node)
: node_(std::move(node))
{
  auto & n = *node_;
  settings_.topics = patterns(n, "topic_whitelist");
  settings_.services = patterns(n, "service_whitelist");
  settings_.parameters = patterns(n, "param_whitelist");
  settings_.client_topics = patterns(n, "client_topic_whitelist");
  const auto capabilities = n.declare_parameter(
    "capabilities", std::vector<std::string>{"clientPublish", "parameters", "services", "assets"});
  settings_.capabilities = {capabilities.begin(), capabilities.end()};
  settings_.max_qos_depth = static_cast<size_t>(n.declare_parameter("max_qos_depth", 10));
  const auto has = [this](const std::string & capability) {
      return settings_.capabilities.count(capability) > 0;
    };

  miti::ServerOptions options;
  options.name = "miti_bridge";
  options.address = n.declare_parameter("address", std::string("0.0.0.0"));
  options.port = static_cast<uint16_t>(n.declare_parameter("port", 8765));
  options.metadata["ROS_DISTRO"] = std::getenv("ROS_DISTRO") ? std::getenv("ROS_DISTRO") : "";
  options.on_subscribe = [this](const miti::Channel & channel) {subscribe(channel);};
  options.on_unsubscribe = [this](const miti::Channel & channel) {unsubscribe(channel);};
  if (has("clientPublish")) {
    options.client_publish.on_advertise = [this](const auto & channel) {client_advertise(channel);};
    options.client_publish.on_message = [this](const auto & channel, auto data, auto size) {
        client_publish(channel, data, size);
      };
    options.client_publish.on_unadvertise =
      [this](const auto & channel) {client_unadvertise(channel);};
  }
  if (has("parameters")) {
    options.get_parameters = [this](const auto & names, auto reply) {
        get_parameters(names, std::move(reply));
      };
  }
  options.services = has("services");
  options.get_schema = [this](const std::string & type) -> std::optional<miti::MessageDefinition> {
      std::lock_guard<std::mutex> lock(mutex_);
      try {
        return definitions_.message(type);
      } catch (const std::exception &) {
        return std::nullopt;
      }
    };
  if (has("assets")) {
    // Every installed package, so package:// URIs of robot descriptions resolve.
    for (const auto & [package, prefix] : ament_index_cpp::get_resources("packages")) {
      options.assets.packages[package] = prefix + "/share/" + package;
    }
    for (const auto & root : n.declare_parameter("asset_roots", std::vector<std::string>{})) {
      options.assets.roots.emplace_back(root);
    }
  }
  options.time = n.get_parameter("use_sim_time").as_bool();
  server_ = miti::Server::create(std::move(options));

  if (n.get_parameter("use_sim_time").as_bool()) {
    clock_ = n.create_subscription<rosgraph_msgs::msg::Clock>(
      "/clock", rclcpp::ClockQoS(), [this](const rosgraph_msgs::msg::Clock & clock) {
        server_->broadcast_time(
          static_cast<miti::Timestamp>(clock.clock.sec) * 1000000000 + clock.clock.nanosec);
      });
  }
  graph_timer_ = n.create_wall_timer(GRAPH_PERIOD, [this] {update_graph();});
  update_graph();
}

Bridge::~Bridge()
{
  server_->stop();
}

void Bridge::update_graph()
{
  update_topics();
  update_services();
  expire();
}

void Bridge::update_topics()
{
  std::set<std::pair<std::string, std::string>> present;
  for (const auto & [name, types] : node_->get_topic_names_and_types()) {
    if (hidden(name) || !matches(settings_.topics, name)) continue;
    for (const auto & type : types) present.emplace(name, full_type_name(type));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto topic = topics_.begin(); topic != topics_.end(); ) {
    if (present.count(topic->first)) {
      ++topic;
      continue;
    }
    server_->remove_channel(*topic->second.channel);
    topic_of_channel_.erase(topic->second.channel->id());
    topic = topics_.erase(topic);
  }
  for (const auto & key : present) {
    if (topics_.count(key)) continue;
    const auto & [name, type] = key;
    miti::ChannelSpec spec{name, {}};
    try {
      spec.message = definitions_.message(type);
    } catch (const std::exception & error) {
      if (unusable_types_.insert(type).second) {
        RCLCPP_WARN(node_->get_logger(), "Not offering topics of type %s: %s", type.c_str(),
          error.what());
      }
      continue;
    }
    // Transient-local publishers (/tf_static, /robot_description) send once: keep the last
    // message of each for clients that subscribe later.
    for (const auto & publisher : node_->get_publishers_info_by_topic(name)) {
      if (publisher.qos_profile().get_rmw_qos_profile().durability ==
        RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL)
      {
        ++spec.latched;
      }
    }
    auto channel = server_->add_channel(std::move(spec));
    topic_of_channel_[channel->id()] = key;
    topics_[key] = {name, type, std::move(channel), nullptr};
  }
}

void Bridge::update_services()
{
  const auto own = std::string(node_->get_fully_qualified_name()) + "/";
  std::map<std::string, std::string> present;
  for (const auto & [name, types] : node_->get_service_names_and_types()) {
    if (hidden(name) || name.rfind(own, 0) == 0 || !matches(settings_.services, name)) continue;
    if (!types.empty()) present[name] = types.front();
  }
  if (!settings_.capabilities.count("services")) return;

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto service = services_.begin(); service != services_.end(); ) {
    const auto found = present.find(service->first);
    if (found != present.end() && found->second == service->second.type) {
      ++service;
      continue;
    }
    server_->remove_service(service->second.id);
    service = services_.erase(service);
  }
  for (const auto & [name, type] : present) {
    if (services_.count(name)) continue;
    miti::ServiceSpec spec{name, type, {}, {}};
    try {
      std::tie(spec.request, spec.response) = definitions_.service(type);
    } catch (const std::exception & error) {
      if (unusable_types_.insert(type).second) {
        RCLCPP_WARN(node_->get_logger(), "Not offering services of type %s: %s", type.c_str(),
          error.what());
      }
      continue;
    }
    const auto id = server_->add_service(
      std::move(spec), [this, name = name](const miti::Bytes & request, auto responder) {
        call_service(name, request, std::move(responder));
      });
    services_[name] = {id, type, nullptr};
  }
}

void Bridge::expire()
{
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & [name, service] : services_) {
    if (service.client) service.client->expire(now);
  }
  for (auto request = parameter_requests_.begin(); request != parameter_requests_.end(); ) {
    std::lock_guard<std::mutex> request_lock((*request)->mutex);
    if ((*request)->answered || (*request)->deadline <= now) {
      (*request)->answer();
      request = parameter_requests_.erase(request);
    } else {
      ++request;
    }
  }
}

void Bridge::subscribe(const miti::Channel & channel)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto key = topic_of_channel_.find(channel.id());
  if (key == topic_of_channel_.end()) return;
  auto & topic = topics_.at(key->second);
  if (topic.subscription) return;

  // Match the publishers: reliable unless one is best effort, transient-local if all are, and
  // deep enough for every publisher's history.
  const auto publishers = node_->get_publishers_info_by_topic(topic.name);
  size_t depth = 0;
  bool best_effort = false;
  bool transient_local = !publishers.empty();
  for (const auto & publisher : publishers) {
    const auto qos = publisher.qos_profile().get_rmw_qos_profile();
    depth += qos.depth;
    best_effort |= qos.reliability == RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    transient_local &= qos.durability == RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  }
  rclcpp::QoS qos(std::clamp<size_t>(depth, 1, settings_.max_qos_depth));
  if (best_effort) qos.best_effort();
  if (transient_local) qos.transient_local();

  try {
    topic.subscription = node_->create_generic_subscription(
      topic.name, topic.type, qos,
      [this, channel = topic.channel](std::shared_ptr<rclcpp::SerializedMessage> message) {
        const auto & raw = message->get_rcl_serialized_message();
        channel->log(
          miti::Bytes(raw.buffer, raw.buffer + raw.buffer_length),
          static_cast<miti::Timestamp>(node_->now().nanoseconds()));
      });
  } catch (const std::exception & error) {
    RCLCPP_ERROR(node_->get_logger(), "Cannot subscribe to %s: %s", topic.name.c_str(),
      error.what());
  }
}

void Bridge::unsubscribe(const miti::Channel & channel)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto key = topic_of_channel_.find(channel.id());
  if (key != topic_of_channel_.end()) topics_.at(key->second).subscription.reset();
}

void Bridge::client_advertise(const miti::ClientChannel & channel)
{
  if (!matches(settings_.client_topics, channel.topic)) {
    RCLCPP_WARN(node_->get_logger(), "Not publishing on %s: not in client_topic_whitelist",
      channel.topic.c_str());
    return;
  }
  try {
    auto publisher = node_->create_generic_publisher(
      channel.topic, full_type_name(channel.message.schema_name), rclcpp::QoS(10));
    std::lock_guard<std::mutex> lock(mutex_);
    publishers_[{channel.client, channel.id}] = std::move(publisher);
  } catch (const std::exception & error) {
    RCLCPP_ERROR(node_->get_logger(), "Cannot publish on %s as %s: %s", channel.topic.c_str(),
      channel.message.schema_name.c_str(), error.what());
  }
}

void Bridge::client_publish(const miti::ClientChannel & channel, const uint8_t * data, size_t size)
{
  rclcpp::GenericPublisher::SharedPtr publisher;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = publishers_.find({channel.client, channel.id});
    if (found == publishers_.end()) return;
    publisher = found->second;
  }
  rclcpp::SerializedMessage message(size);
  auto & raw = message.get_rcl_serialized_message();
  std::copy(data, data + size, raw.buffer);
  raw.buffer_length = size;
  publisher->publish(message);
}

void Bridge::client_unadvertise(const miti::ClientChannel & channel)
{
  std::lock_guard<std::mutex> lock(mutex_);
  publishers_.erase({channel.client, channel.id});
}

void Bridge::get_parameters(
  const std::vector<std::string> & names, std::function<void(std::vector<miti::Parameter>)> reply)
{
  std::map<std::string, std::vector<std::string>> by_node;
  for (const auto & name : names) {
    const auto [node, parameter] = split_parameter(name);
    if (!node.empty() && matches(settings_.parameters, name)) by_node[node].push_back(parameter);
  }
  auto request = std::make_shared<ParameterRequest>();
  request->reply = std::move(reply);
  request->deadline = std::chrono::steady_clock::now() + PARAMETER_TIMEOUT;
  request->waiting = by_node.size() + 1;  // + 1 until every node has been asked

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & [node, parameters] : by_node) {
    auto & client = parameter_clients_[node];
    if (!client) client = std::make_shared<rclcpp::AsyncParametersClient>(node_, node);
    if (!client->service_is_ready()) {
      request->done({});
      continue;
    }
    client->get_parameters(
      parameters, [request, node = node](std::shared_future<std::vector<rclcpp::Parameter>> got) {
        std::vector<miti::Parameter> found;
        try {
          for (const auto & parameter : got.get()) {
            if (auto converted = value(parameter)) {
              found.push_back({node + "." + parameter.get_name(), std::move(*converted)});
            }
          }
        } catch (const std::exception &) {
          // The node went away: it has none of them.
        }
        request->done(std::move(found));
      });
  }
  request->done({});
  parameter_requests_.push_back(std::move(request));
}

void Bridge::call_service(
  const std::string & name, const miti::Bytes & request, miti::ServiceResponder responder)
{
  std::shared_ptr<GenericClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto service = services_.find(name);
    if (service == services_.end()) {
      responder.fail("Service " + name + " is gone");
      return;
    }
    try {
      if (!service->second.client) {
        service->second.client = GenericClient::create(*node_, name, service->second.type);
      }
    } catch (const std::exception & error) {
      responder.fail(error.what());
      return;
    }
    client = service->second.client;
  }
  client->call(
    request, [responder](GenericClient::Result result) {
      if (auto * response = std::get_if<std::vector<uint8_t>>(&result)) {
        responder.respond(std::move(*response));
      } else {
        responder.fail(std::get<std::string>(result));
      }
    }, SERVICE_TIMEOUT);
}

}  // namespace miti_bridge
