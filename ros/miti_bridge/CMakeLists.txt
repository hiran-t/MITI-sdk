cmake_minimum_required(VERSION 3.16)
project(miti_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(libmiti 0.1 REQUIRED)
find_package(rclcpp REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rcpputils REQUIRED)
find_package(rmw REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)
find_package(rosgraph_msgs REQUIRED)

add_library(miti_bridge_core STATIC
  src/bridge.cpp
  src/definitions.cpp
  src/generic_client.cpp)
target_include_directories(miti_bridge_core PUBLIC src)
target_compile_options(miti_bridge_core PRIVATE -Wall -Wextra)
target_link_libraries(miti_bridge_core PUBLIC
  miti::miti
  rclcpp::rclcpp
  ament_index_cpp::ament_index_cpp
  rcpputils::rcpputils
  rmw::rmw
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp
  ${rosgraph_msgs_TARGETS})

add_executable(miti_bridge src/main.cpp)
target_link_libraries(miti_bridge PRIVATE miti_bridge_core)
install(TARGETS miti_bridge DESTINATION lib/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(definitions_test test/definitions_test.cpp)
  target_link_libraries(definitions_test miti_bridge_core)
endif()

ament_package()
