find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(miti_tests
  assets_test.cpp
  protocol_test.cpp
  scheduler_test.cpp
  server_test.cpp)
# The tests check internals too, so they see src/ and link what the library links.
target_include_directories(miti_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(miti_tests PRIVATE
  miti Boost::headers nlohmann_json::nlohmann_json Threads::Threads GTest::gtest_main)
gtest_discover_tests(miti_tests)
