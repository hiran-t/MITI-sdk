#include "generic_client.hpp"

#include <map>
#include <mutex>

#include <rcl/client.h>
#include <rclcpp/typesupport_helpers.hpp>
#include <rcpputils/shared_library.hpp>
#include <rmw/rmw.h>
#include <rmw/serialized_message.h>
#include <rosidl_typesupport_introspection_cpp/message_introspection.hpp>

namespace miti_bridge
{

namespace
{

using MessageMembers = rosidl_typesupport_introspection_cpp::MessageMembers;

/// Calls the exported `<prefix><package>__srv__<name>` function that returns a type support.
template<typename Support>
const Support * lookup(
  rcpputils::SharedLibrary & library, const std::string & prefix, const std::string & package,
  const std::string & name)
{
  using Get = const Support * (*)();
  const auto symbol = prefix + package + "__srv__" + name;
  if (!library.has_symbol(symbol)) throw std::runtime_error("No type support symbol " + symbol);
  return reinterpret_cast<Get>(library.get_symbol(symbol))();
}

/// A type-support library, loaded once and kept until the process ends: rcl and the middleware may
/// still use the type support while a client is being destroyed, after the client's own members.
rcpputils::SharedLibrary & library(const std::string & type, const std::string & typesupport)
{
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<rcpputils::SharedLibrary>> loaded;
  std::lock_guard<std::mutex> lock(mutex);
  const auto package = type.substr(0, type.find('/'));
  auto & kept = loaded[package + " " + typesupport];
  if (!kept) kept = rclcpp::get_typesupport_library(type, typesupport);
  return *kept;
}

/// A serialized message that frees its buffer.
struct Serialized
{
  explicit Serialized(size_t capacity)
  {
    auto allocator = rcutils_get_default_allocator();
    if (rmw_serialized_message_init(&message, capacity, &allocator) != RMW_RET_OK) {
      throw std::runtime_error("Cannot allocate a serialized message");
    }
  }
  ~Serialized()
  {
    if (rmw_serialized_message_fini(&message) != RMW_RET_OK) rcutils_reset_error();
  }
  Serialized(const Serialized &) = delete;
  Serialized & operator=(const Serialized &) = delete;
  rmw_serialized_message_t message = rmw_get_zero_initialized_serialized_message();
};

}  // namespace

std::shared_ptr<GenericClient> GenericClient::create(
  rclcpp::Node & node, const std::string & service, const std::string & type)
{
  std::shared_ptr<GenericClient> client(new GenericClient(node, service, type));
  node.get_node_services_interface()->add_client(client, nullptr);
  return client;
}

GenericClient::GenericClient(
  rclcpp::Node & node, const std::string & service, const std::string & type)
: rclcpp::ClientBase(node.get_node_base_interface().get(), node.get_node_graph_interface()),
  service_(service)
{
  const auto first = type.find('/');
  const auto last = type.rfind('/');
  if (first == std::string::npos || type.substr(first + 1, last - first - 1) != "srv") {
    throw std::invalid_argument("Expected pkg/srv/Type, got " + type);
  }
  const auto package = type.substr(0, first);
  const auto name = type.substr(last + 1);
  // rclcpp's helper for message handles is deprecated on Jazzy and the one for services does not
  // exist on Humble: every handle is looked up by its symbol, the same on both.
  auto & cpp_library = library(type, "rosidl_typesupport_cpp");
  auto & introspection_library = library(type, "rosidl_typesupport_introspection_cpp");
  const std::string cpp = "rosidl_typesupport_cpp__get_";
  const std::string introspection = "rosidl_typesupport_introspection_cpp__get_";
  const auto * service_support = lookup<rosidl_service_type_support_t>(
    cpp_library, cpp + "service_type_support_handle__", package, name);
  request_cpp_ = lookup<rosidl_message_type_support_t>(
    cpp_library, cpp + "message_type_support_handle__", package, name + "_Request");
  response_cpp_ = lookup<rosidl_message_type_support_t>(
    cpp_library, cpp + "message_type_support_handle__", package, name + "_Response");
  request_layout_ = lookup<rosidl_message_type_support_t>(
    introspection_library, introspection + "message_type_support_handle__", package,
    name + "_Request");
  response_layout_ = lookup<rosidl_message_type_support_t>(
    introspection_library, introspection + "message_type_support_handle__", package,
    name + "_Response");

  const rcl_client_options_t options = rcl_client_get_default_options();
  if (rcl_client_init(get_client_handle().get(), get_rcl_node_handle(), service_support,
    service.c_str(), &options) != RCL_RET_OK)
  {
    const std::string error = rcl_get_error_string().str;
    rcl_reset_error();
    throw std::runtime_error("Cannot create a client for " + service + ": " + error);
  }
}

std::shared_ptr<void> GenericClient::allocate(const rosidl_message_type_support_t * layout) const
{
  const auto * members = static_cast<const MessageMembers *>(layout->data);
  void * memory = ::operator new(members->size_of_);
  members->init_function(memory, rosidl_runtime_cpp::MessageInitialization::ALL);
  return std::shared_ptr<void>(memory, [members](void * message) {
             members->fini_function(message);
             ::operator delete(message);
           });
}

void GenericClient::call(
  const std::vector<uint8_t> & request, Callback done,
  std::chrono::steady_clock::duration timeout)
{
  if (!service_is_ready()) {
    done(std::string("Service " + service_ + " is not available"));
    return;
  }
  Serialized serialized(request.size());
  std::copy(request.begin(), request.end(), serialized.message.buffer);
  serialized.message.buffer_length = request.size();
  const auto message = allocate(request_layout_);
  if (rmw_deserialize(&serialized.message, request_cpp_, message.get()) != RMW_RET_OK) {
    rcutils_reset_error();
    done(std::string("The request is not a valid CDR request for " + service_));
    return;
  }
  // Held while sending, so a response cannot arrive before its call is recorded.
  std::unique_lock<std::mutex> lock(mutex_);
  int64_t sequence = 0;
  if (rcl_send_request(get_client_handle().get(), message.get(), &sequence) != RCL_RET_OK) {
    const std::string error = rcl_get_error_string().str;
    rcl_reset_error();
    lock.unlock();
    done(std::string("Cannot send the request: " + error));
    return;
  }
  pending_[sequence] = {std::move(done), std::chrono::steady_clock::now() + timeout};
}

void GenericClient::expire(std::chrono::steady_clock::time_point now)
{
  std::vector<Callback> expired;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto call = pending_.begin(); call != pending_.end(); ) {
      if (call->second.deadline > now) {
        ++call;
        continue;
      }
      expired.push_back(std::move(call->second.done));
      call = pending_.erase(call);
    }
  }
  for (auto & done : expired) done(std::string("No response from " + service_ + " in time"));
}

std::shared_ptr<void> GenericClient::create_response()
{
  return allocate(response_layout_);
}

std::shared_ptr<rmw_request_id_t> GenericClient::create_request_header()
{
  return std::make_shared<rmw_request_id_t>();
}

void GenericClient::handle_response(
  std::shared_ptr<rmw_request_id_t> header, std::shared_ptr<void> response)
{
  Callback done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto call = pending_.find(header->sequence_number);
    if (call == pending_.end()) return;  // timed out already
    done = std::move(call->second.done);
    pending_.erase(call);
  }
  Serialized serialized(0);
  if (rmw_serialize(response.get(), response_cpp_, &serialized.message) != RMW_RET_OK) {
    rcutils_reset_error();
    done(std::string("Cannot serialize the response of " + service_));
    return;
  }
  const auto & bytes = serialized.message;
  done(std::vector<uint8_t>(bytes.buffer, bytes.buffer + bytes.buffer_length));
}

}  // namespace miti_bridge
