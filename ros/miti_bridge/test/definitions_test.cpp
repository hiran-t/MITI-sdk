#include <gtest/gtest.h>

#include "definitions.hpp"

using miti_bridge::Definitions;
using miti_bridge::full_type_name;

namespace
{

const std::string SEPARATOR = "\n" + std::string(80, '=') + "\n";

size_t count(const std::string & text, const std::string & part)
{
  size_t found = 0;
  for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++found;
  return found;
}

}  // namespace

TEST(Definitions, CompletesTypeNames)
{
  EXPECT_EQ(full_type_name("std_msgs/String"), "std_msgs/msg/String");
  EXPECT_EQ(full_type_name("std_msgs/msg/String"), "std_msgs/msg/String");
  EXPECT_EQ(full_type_name("std_srvs/srv/SetBool"), "std_srvs/srv/SetBool");
  EXPECT_THROW(full_type_name("String"), std::runtime_error);
}

TEST(Definitions, IncludesEveryUsedTypeOnce)
{
  Definitions definitions;
  const auto & image = definitions.message("sensor_msgs/msg/Image");
  EXPECT_EQ(image.encoding, "cdr");
  EXPECT_EQ(image.schema_name, "sensor_msgs/msg/Image");
  EXPECT_EQ(image.schema_encoding, "ros2msg");
  EXPECT_EQ(image.schema.find("std_msgs/Header header"), image.schema.find("std_msgs/Header"));
  EXPECT_EQ(count(image.schema, SEPARATOR + "MSG: std_msgs/Header\n"), 1u) << image.schema;
  EXPECT_EQ(count(image.schema, SEPARATOR + "MSG: builtin_interfaces/Time\n"), 1u);

  // Header and Time are used through TransformStamped, inside an array: each still once.
  const auto & tf = definitions.message("tf2_msgs/TFMessage");
  for (const auto * type : {"geometry_msgs/TransformStamped", "std_msgs/Header",
      "builtin_interfaces/Time", "geometry_msgs/Transform", "geometry_msgs/Vector3",
      "geometry_msgs/Quaternion"})
  {
    EXPECT_EQ(count(tf.schema, SEPARATOR + "MSG: " + type + "\n"), 1u) << type;
  }
}

TEST(Definitions, SplitsServicesIntoRequestAndResponse)
{
  Definitions definitions;
  const auto [request, response] = definitions.service("std_srvs/srv/SetBool");
  EXPECT_EQ(request.schema_name, "std_srvs/srv/SetBool_Request");
  EXPECT_EQ(response.schema_name, "std_srvs/srv/SetBool_Response");
  EXPECT_NE(request.schema.find("bool data"), std::string::npos) << request.schema;
  EXPECT_EQ(request.schema.find("success"), std::string::npos);
  EXPECT_NE(response.schema.find("bool success"), std::string::npos);
  EXPECT_NE(response.schema.find("string message"), std::string::npos);
}

TEST(Definitions, ReportsWhatCannotBeFound)
{
  Definitions definitions;
  EXPECT_THROW(definitions.message("no_such_msgs/msg/Thing"), std::runtime_error);
  EXPECT_THROW(definitions.message("std_msgs/msg/NoSuchType"), std::runtime_error);
  EXPECT_THROW(definitions.message("std_srvs/srv/SetBool"), std::runtime_error);
  EXPECT_THROW(definitions.service("std_msgs/msg/String"), std::runtime_error);
}
