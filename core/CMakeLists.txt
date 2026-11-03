cmake_minimum_required(VERSION 3.16)
project(libmiti VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MITI_BUILD_TESTS "Build libmiti's tests" ON)
option(MITI_BUILD_EXAMPLES "Build libmiti's examples" ON)

# Boost 1.74 (Ubuntu 22.04) is the oldest supported: Asio and Beast, both header-only. Boost's own
# CMake config is used where CMake no longer has FindBoost.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.74 REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
# TurboJPEG, for the jpeg transform: Ubuntu's libturbojpeg0-dev and conda's libjpeg-turbo both
# install a pkg-config file, and neither a CMake package everywhere.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(miti
  src/assets.cpp
  src/protocol.cpp
  src/scheduler.cpp
  src/server.cpp
  src/session.cpp
  src/transforms.cpp)
add_library(miti::miti ALIAS miti)
target_include_directories(miti
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src)
# Boost and nlohmann/json stay private: programs see only include/miti.
target_link_libraries(miti
  PRIVATE
    $<BUILD_INTERFACE:Boost::headers> $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
    $<BUILD_INTERFACE:PkgConfig::TURBOJPEG>
  PUBLIC Threads::Threads)
# A static libmiti needs TurboJPEG where it is linked.
if(NOT BUILD_SHARED_LIBS)
  target_link_libraries(miti INTERFACE $<INSTALL_INTERFACE:${TURBOJPEG_LINK_LIBRARIES}>)
endif()
target_compile_definitions(miti PRIVATE MITI_VERSION="${PROJECT_VERSION}")
target_compile_options(miti PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>)

# find_package(libmiti) then link miti::miti.
install(TARGETS miti EXPORT libmitiTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT libmitiTargets NAMESPACE miti:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmiti)
configure_package_config_file(cmake/libmitiConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/libmitiConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmiti)
# 0.x: a new minor version may change the API.
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libmitiConfigVersion.cmake
  COMPATIBILITY SameMinorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libmitiConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/libmitiConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmiti)

if(MITI_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MITI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
