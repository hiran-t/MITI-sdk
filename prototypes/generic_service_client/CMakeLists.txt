cmake_minimum_required(VERSION 3.16)
project(generic_service_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rclcpp REQUIRED)
find_package(rcpputils REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)

add_executable(generic_service_call main.cpp)
target_compile_options(generic_service_call PRIVATE -Wall -Wextra)
target_link_libraries(generic_service_call
  rclcpp::rclcpp
  rcpputils::rcpputils
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp)
