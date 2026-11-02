// What a subscription's transform does to each message (protocol.md, Transforms).

#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "miti/server.hpp"

namespace miti::transforms
{

struct Transform
{
  enum class Kind { jpeg, reduce };
  Kind kind = Kind::jpeg;
  // jpeg
  int quality = 80;
  uint32_t max_width = 0;  // 0: keep the width
  // reduce
  std::vector<std::string> fields;  // empty: keep every field
  double voxel = 0;
  uint32_t stride = 0;
  uint32_t max_points = 0;

  /// Equal for transforms that make the same output from the same input.
  std::string key() const;
};

/// A transform from its JSON, or why it is not one.
std::variant<Transform, std::string> parse(const nlohmann::json & json);

/// The message type the transform takes: sensor_msgs/msg/Image or sensor_msgs/msg/PointCloud2.
const char * input_type(Transform::Kind kind);

/// The definition of what the transform makes, when it is another type than its input.
std::optional<MessageDefinition> output_definition(const Transform & transform);

/// The transformed message, or why there is none: such a message is dropped.
using Result = std::variant<Bytes, std::string>;
Result apply(const Transform & transform, const Bytes & input);

}  // namespace miti::transforms
