// Prototype for MITI's generic service calls: call a ROS 2 service of any type with a CDR request
// and get the CDR response, using only what Humble provides. Humble's rclcpp has no
// GenericClient and no service type-support helper (Jazzy has both), so this works on rcl:
//
//   1. load the service's type support by name from lib<pkg>__rosidl_typesupport_cpp;
//   2. deserialize the request bytes into a request message allocated through the
//      introspection type support (its size and init function);
//   3. send it with rcl_send_request and take the response with rcl_take_response_with_info;
//   4. serialize the response back to CDR.
//
//   generic_service_call <service> <pkg/srv/Type> <request CDR as hex>
// prints the response CDR as hex. miti_bridge's version runs on its executor instead of polling.

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rcl/client.h>
#include <rcl/graph.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/typesupport_helpers.hpp>
#include <rcpputils/shared_library.hpp>
#include <rmw/serialized_message.h>
#include <rmw/rmw.h>
#include <rosidl_typesupport_introspection_cpp/message_introspection.hpp>

using namespace std::chrono_literals;
using MessageMembers = rosidl_typesupport_introspection_cpp::MessageMembers;

namespace
{

/// A service type and its type-support libraries. Handles are looked up by their exported
/// symbol names for messages too: rclcpp's helper for them is deprecated on Jazzy and the one for
/// services does not exist on Humble, so one lookup serves both distros.
class ServiceType
{
public:
  explicit ServiceType(const std::string & type)
  {
    const auto first = type.find('/');
    const auto last = type.rfind('/');
    if (first == std::string::npos || type.substr(first + 1, last - first - 1) != "srv")
      throw std::invalid_argument("Expected pkg/srv/Type, got " + type);
    package_ = type.substr(0, first);
    name_ = type.substr(last + 1);
    cpp_ = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
    introspection_ = rclcpp::get_typesupport_library(type, "rosidl_typesupport_introspection_cpp");
  }

  std::string message(const std::string & part) const
  {
    return package_ + "/srv/" + name_ + "_" + part;  // std_srvs/srv/SetBool_Request
  }

  const rosidl_service_type_support_t * service_support() const
  {
    return lookup<rosidl_service_type_support_t>(
      *cpp_, "rosidl_typesupport_cpp__get_service_type_support_handle__", name_);
  }

  /// `typesupport` is rosidl_typesupport_cpp or rosidl_typesupport_introspection_cpp.
  const rosidl_message_type_support_t * message_support(
    const std::string & typesupport, const std::string & part) const
  {
    auto & library = typesupport == "rosidl_typesupport_cpp" ? *cpp_ : *introspection_;
    return lookup<rosidl_message_type_support_t>(
      library, typesupport + "__get_message_type_support_handle__", name_ + "_" + part);
  }

private:
  template<typename Support>
  const Support * lookup(
    rcpputils::SharedLibrary & library, const std::string & prefix, const std::string & type) const
  {
    using Get = const Support * (*)();
    const auto symbol = prefix + package_ + "__srv__" + type;
    if (!library.has_symbol(symbol)) throw std::runtime_error("No type support symbol " + symbol);
    return reinterpret_cast<Get>(library.get_symbol(symbol))();
  }

  std::string package_, name_;  // std_srvs, SetBool
  std::shared_ptr<rcpputils::SharedLibrary> cpp_, introspection_;
};

/// A message of a type known only at run time, laid out by its introspection type support.
class DynamicMessage
{
public:
  explicit DynamicMessage(const rosidl_message_type_support_t * introspection)
  : members_(static_cast<const MessageMembers *>(introspection->data)),
    buffer_(members_->size_of_)
  {
    members_->init_function(buffer_.data(), rosidl_runtime_cpp::MessageInitialization::ALL);
  }
  ~DynamicMessage() {members_->fini_function(buffer_.data());}
  DynamicMessage(const DynamicMessage &) = delete;
  DynamicMessage & operator=(const DynamicMessage &) = delete;
  void * data() {return buffer_.data();}

private:
  const MessageMembers * members_;
  std::vector<unsigned char> buffer_;
};

class SerializedMessage
{
public:
  explicit SerializedMessage(size_t capacity = 0)
  {
    auto allocator = rcutils_get_default_allocator();
    if (rmw_serialized_message_init(&message, capacity, &allocator) != RMW_RET_OK)
      throw std::runtime_error("Cannot allocate a serialized message");
  }
  ~SerializedMessage() {(void)rmw_serialized_message_fini(&message);}
  SerializedMessage(const SerializedMessage &) = delete;
  SerializedMessage & operator=(const SerializedMessage &) = delete;
  rmw_serialized_message_t message = rmw_get_zero_initialized_serialized_message();
};

std::vector<uint8_t> from_hex(const std::string & hex)
{
  if (hex.size() % 2) throw std::invalid_argument("Odd number of hex digits");
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < hex.size(); i += 2) bytes.push_back(std::stoi(hex.substr(i, 2), nullptr, 16));
  return bytes;
}

std::string to_hex(const uint8_t * bytes, size_t length)
{
  static const char digits[] = "0123456789abcdef";
  std::string hex;
  for (size_t i = 0; i < length; ++i) {
    hex += digits[bytes[i] >> 4];
    hex += digits[bytes[i] & 0xf];
  }
  return hex;
}

void check(rcl_ret_t ret, const char * what)
{
  if (ret != RCL_RET_OK) {
    const std::string error = rcl_get_error_string().str;
    rcl_reset_error();
    throw std::runtime_error(std::string(what) + ": " + error);
  }
}

std::vector<uint8_t> call(
  rclcpp::Node & node, const std::string & service, const std::string & type,
  const std::vector<uint8_t> & request_cdr, std::chrono::milliseconds timeout)
{
  const ServiceType service_type(type);
  const auto request_cpp = service_type.message_support("rosidl_typesupport_cpp", "Request");
  const auto response_cpp = service_type.message_support("rosidl_typesupport_cpp", "Response");
  const auto request_layout =
    service_type.message_support("rosidl_typesupport_introspection_cpp", "Request");
  const auto response_layout =
    service_type.message_support("rosidl_typesupport_introspection_cpp", "Response");

  rcl_node_t * rcl_node = node.get_node_base_interface()->get_rcl_node_handle();
  rcl_client_t client = rcl_get_zero_initialized_client();
  const rcl_client_options_t options = rcl_client_get_default_options();
  check(
    rcl_client_init(&client, rcl_node, service_type.service_support(), service.c_str(), &options),
    "rcl_client_init");
  struct ClientFini
  {
    rcl_client_t * client; rcl_node_t * node;
    ~ClientFini()
    {
      if (rcl_client_fini(client, node) != RCL_RET_OK) rcl_reset_error();
    }
  } fini{&client, rcl_node};

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (bool available = false; !available; std::this_thread::sleep_for(10ms)) {
    check(rcl_service_server_is_available(rcl_node, &client, &available), "server availability");
    if (!available && std::chrono::steady_clock::now() > deadline)
      throw std::runtime_error(service + " is not available");
  }

  SerializedMessage serialized_request(request_cdr.size());
  std::copy(request_cdr.begin(), request_cdr.end(), serialized_request.message.buffer);
  serialized_request.message.buffer_length = request_cdr.size();
  DynamicMessage request(request_layout);
  if (rmw_deserialize(&serialized_request.message, request_cpp, request.data()) != RMW_RET_OK)
    throw std::runtime_error("The request does not deserialize as " + service_type.message("Request"));

  int64_t sequence = 0;
  check(rcl_send_request(&client, request.data(), &sequence), "rcl_send_request");

  DynamicMessage response(response_layout);
  for (;; std::this_thread::sleep_for(5ms)) {
    rmw_service_info_t info;
    const rcl_ret_t ret = rcl_take_response_with_info(&client, &info, response.data());
    if (ret == RCL_RET_OK && info.request_id.sequence_number == sequence) break;
    if (ret != RCL_RET_OK && ret != RCL_RET_CLIENT_TAKE_FAILED) check(ret, "rcl_take_response");
    if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("No response in time");
  }

  SerializedMessage serialized_response;
  if (rmw_serialize(response.data(), response_cpp, &serialized_response.message) != RMW_RET_OK)
    throw std::runtime_error("Cannot serialize the response");
  const auto & out = serialized_response.message;
  return {out.buffer, out.buffer + out.buffer_length};
}

}  // namespace

int main(int argc, char ** argv)
{
  if (argc != 4) {
    std::fprintf(stderr, "Usage: %s <service> <pkg/srv/Type> <request CDR as hex>\n", argv[0]);
    return 2;
  }
  rclcpp::init(1, argv);
  int status = 0;
  try {
    auto node = std::make_shared<rclcpp::Node>("generic_service_call");
    const auto response = call(*node, argv[1], argv[2], from_hex(argv[3]), 10s);
    std::printf("%s\n", to_hex(response.data(), response.size()).c_str());
  } catch (const std::exception & error) {
    std::fprintf(stderr, "generic_service_call: %s\n", error.what());
    status = 1;
  }
  rclcpp::shutdown();
  return status;
}
