// miti_bridge: serves this ROS 2 graph to MITI.
//   ros2 run miti_bridge miti_bridge --ros-args -p port:=8765

#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "bridge.hpp"

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("miti_bridge");
  std::unique_ptr<miti_bridge::Bridge> bridge;
  try {
    bridge = std::make_unique<miti_bridge::Bridge>(node);
  } catch (const std::exception & error) {
    RCLCPP_FATAL(node->get_logger(), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  RCLCPP_INFO(node->get_logger(), "Serving MITI on port %u (miti.bridge.v1)", bridge->port());
  rclcpp::spin(node);
  bridge.reset();
  rclcpp::shutdown();
  return 0;
}
