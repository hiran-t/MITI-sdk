#include "transforms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_set>

#include <turbojpeg.h>

#include "cdr.hpp"

namespace miti::transforms
{

namespace
{

const std::string SEPARATOR = "\n" + std::string(80, '=') + "\n";
const std::string COMPRESSED_IMAGE =
  "std_msgs/Header header\nstring format\nuint8[] data" + SEPARATOR +
  "MSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id" + SEPARATOR +
  "MSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec";

/// A TurboJPEG compressor per thread: transforms run on several.
class Compressor
{
public:
  Compressor() : handle_(tjInitCompress()) {}
  ~Compressor() {if (handle_) tjDestroy(handle_);}
  Compressor(const Compressor &) = delete;
  Compressor & operator=(const Compressor &) = delete;
  tjhandle get() const {return handle_;}

private:
  tjhandle handle_;
};

struct PixelFormat
{
  int format;
  uint32_t channels;
  int subsampling;
};

const std::map<std::string, PixelFormat> JPEG_ENCODINGS{
  {"rgb8", {TJPF_RGB, 3, TJSAMP_420}},
  {"bgr8", {TJPF_BGR, 3, TJSAMP_420}},
  {"rgba8", {TJPF_RGBA, 4, TJSAMP_420}},
  {"bgra8", {TJPF_BGRA, 4, TJSAMP_420}},
  {"mono8", {TJPF_GRAY, 1, TJSAMP_GRAY}},
};

/// Averages each destination pixel over the source pixels it covers.
std::vector<uint8_t> shrink(
  const uint8_t * source, uint32_t width, uint32_t height, uint32_t step, uint32_t channels,
  uint32_t to_width, uint32_t to_height)
{
  std::vector<uint8_t> out(size_t(to_width) * to_height * channels);
  for (uint32_t y = 0; y < to_height; ++y) {
    const size_t y0 = size_t(y) * height / to_height;
    const size_t y1 = std::max(y0 + 1, size_t(y + 1) * height / to_height);
    for (uint32_t x = 0; x < to_width; ++x) {
      const size_t x0 = size_t(x) * width / to_width;
      const size_t x1 = std::max(x0 + 1, size_t(x + 1) * width / to_width);
      for (uint32_t c = 0; c < channels; ++c) {
        uint64_t sum = 0;
        for (size_t yy = y0; yy < y1; ++yy) {
          for (size_t xx = x0; xx < x1; ++xx) sum += source[yy * step + xx * channels + c];
        }
        out[(size_t(y) * to_width + x) * channels + c] =
          static_cast<uint8_t>(sum / ((y1 - y0) * (x1 - x0)));
      }
    }
  }
  return out;
}

Result jpeg(const Transform & transform, const Bytes & input)
{
  cdr::Reader reader(input.data(), input.size());
  const auto header = cdr::read_header(reader);
  const uint32_t height = reader.u32();
  const uint32_t width = reader.u32();
  const auto encoding = reader.string();
  reader.u8();  // is_bigendian: 8-bit encodings have no byte order
  const uint32_t step = reader.u32();
  const auto [data, size] = reader.bytes();

  const auto format = JPEG_ENCODINGS.find(encoding);
  if (format == JPEG_ENCODINGS.end()) {
    return "jpeg cannot encode " + encoding + " images; their messages are dropped";
  }
  const auto [pixel_format, channels, subsampling] = format->second;
  if (width == 0 || height == 0) return std::string("The image is empty");
  if (step < width * channels || size < size_t(step) * height) {
    return std::string("The image's data is shorter than its size");
  }

  const uint8_t * pixels = data;
  uint32_t out_width = width;
  uint32_t out_height = height;
  uint32_t pitch = step;
  std::vector<uint8_t> shrunk;
  if (transform.max_width && width > transform.max_width) {
    out_width = transform.max_width;
    out_height = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::lround(double(height) * out_width / width)));
    shrunk = shrink(data, width, height, step, channels, out_width, out_height);
    pixels = shrunk.data();
    pitch = out_width * channels;
  }

  thread_local Compressor compressor;
  unsigned char * compressed = nullptr;
  unsigned long compressed_size = 0;
  if (tjCompress2(compressor.get(), pixels, static_cast<int>(out_width), static_cast<int>(pitch),
    static_cast<int>(out_height), pixel_format, &compressed, &compressed_size, subsampling,
    transform.quality, TJFLAG_FASTDCT) != 0)
  {
    std::string error = tjGetErrorStr2(compressor.get());
    tjFree(compressed);
    return "jpeg failed: " + error;
  }
  cdr::Writer out;
  cdr::write_header(out, header);
  out.string("jpeg").bytes(compressed, compressed_size);
  tjFree(compressed);
  return out.take();
}

struct Field
{
  std::string name;
  uint32_t offset;
  uint8_t datatype;
  uint32_t count;
};

size_t datatype_size(uint8_t datatype)
{
  static const size_t sizes[] = {0, 1, 1, 2, 2, 4, 4, 4, 8};
  return datatype < 9 ? sizes[datatype] : 0;
}

double coordinate(const uint8_t * point, const Field & field)
{
  if (field.datatype == 7) {
    float value;
    std::memcpy(&value, point + field.offset, 4);
    return value;
  }
  double value;
  std::memcpy(&value, point + field.offset, 8);
  return value;
}

struct VoxelHash
{
  size_t operator()(const std::array<int64_t, 3> & cell) const
  {
    return std::hash<int64_t>()(cell[0]) * 73856093 ^ std::hash<int64_t>()(cell[1]) * 19349663 ^
           std::hash<int64_t>()(cell[2]) * 83492791;
  }
};

Result reduce(const Transform & transform, const Bytes & input)
{
  cdr::Reader reader(input.data(), input.size());
  const auto header = cdr::read_header(reader);
  const uint32_t height = reader.u32();
  const uint32_t width = reader.u32();
  std::vector<Field> fields(reader.u32());
  for (auto & field : fields) {
    field.name = reader.string();
    field.offset = reader.u32();
    field.datatype = reader.u8();
    field.count = reader.u32();
  }
  const bool big_endian = reader.boolean();
  const uint32_t point_step = reader.u32();
  const uint32_t row_step = reader.u32();
  const auto [data, size] = reader.bytes();
  if (big_endian) return std::string("reduce cannot read big-endian point clouds");

  const auto find = [&fields](const std::string & name) -> const Field * {
      for (const auto & field : fields) {
        if (field.name == name && field.count == 1 && (field.datatype == 7 || field.datatype == 8)) {
          return &field;
        }
      }
      return nullptr;
    };
  const Field * x = find("x");
  const Field * y = find("y");
  const Field * z = find("z");
  if (!x || !y || !z) return std::string("reduce needs float x, y and z fields");
  for (const auto & field : fields) {
    if (!datatype_size(field.datatype) ||
      field.offset + datatype_size(field.datatype) * field.count > point_step)
    {
      return "Field " + field.name + " does not fit in a point";
    }
  }
  if (height && (size_t(height - 1) * row_step + size_t(width) * point_step > size ||
    size_t(width) * point_step > row_step))
  {
    return std::string("The point cloud's data is shorter than its size");
  }

  // The kept fields, repacked without padding; `from` is where each comes from in a point.
  std::vector<Field> kept;
  std::vector<uint32_t> from;
  uint32_t kept_step = 0;
  for (const auto & field : fields) {
    const bool wanted = transform.fields.empty() || field.name == "x" || field.name == "y" ||
      field.name == "z" ||
      std::find(transform.fields.begin(), transform.fields.end(), field.name) !=
      transform.fields.end();
    if (!wanted) continue;
    kept.push_back({field.name, kept_step, field.datatype, field.count});
    from.push_back(field.offset);
    kept_step += static_cast<uint32_t>(datatype_size(field.datatype) * field.count);
  }

  // Finite points, the first in each voxel.
  std::vector<const uint8_t *> points;
  std::unordered_set<std::array<int64_t, 3>, VoxelHash> voxels;
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t column = 0; column < width; ++column) {
      const uint8_t * point = data + size_t(row) * row_step + size_t(column) * point_step;
      const double px = coordinate(point, *x);
      const double py = coordinate(point, *y);
      const double pz = coordinate(point, *z);
      if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
      if (transform.voxel > 0) {
        const std::array<int64_t, 3> cell{
          static_cast<int64_t>(std::floor(px / transform.voxel)),
          static_cast<int64_t>(std::floor(py / transform.voxel)),
          static_cast<int64_t>(std::floor(pz / transform.voxel))};
        if (!voxels.insert(cell).second) continue;
      }
      points.push_back(point);
    }
  }
  if (transform.stride > 1) {
    size_t to = 0;
    for (size_t from = 0; from < points.size(); from += transform.stride) points[to++] = points[from];
    points.resize(to);
  }
  if (transform.max_points && points.size() > transform.max_points) {
    std::vector<const uint8_t *> even(transform.max_points);
    for (size_t i = 0; i < even.size(); ++i) even[i] = points[i * points.size() / even.size()];
    points = std::move(even);
  }

  Bytes packed(points.size() * kept_step);
  uint8_t * out_point = packed.data();
  for (const uint8_t * point : points) {
    for (size_t i = 0; i < kept.size(); ++i) {
      std::memcpy(out_point + kept[i].offset, point + from[i],
        datatype_size(kept[i].datatype) * kept[i].count);
    }
    out_point += kept_step;
  }

  cdr::Writer out;
  cdr::write_header(out, header);
  out.u32(1).u32(static_cast<uint32_t>(points.size())).u32(static_cast<uint32_t>(kept.size()));
  for (const auto & field : kept) {
    out.string(field.name).u32(field.offset).u8(field.datatype).u32(field.count);
  }
  out.boolean(false).u32(kept_step).u32(kept_step * static_cast<uint32_t>(points.size()));
  out.bytes(packed.data(), packed.size()).boolean(true);
  return out.take();
}

}  // namespace

std::string Transform::key() const
{
  if (kind == Kind::jpeg) {
    return "jpeg/" + std::to_string(quality) + "/" + std::to_string(max_width);
  }
  std::string names;
  for (const auto & field : fields) names += field + ",";
  char voxel_text[32];
  std::snprintf(voxel_text, sizeof(voxel_text), "%.17g", voxel);
  return "reduce/" + names + "/" + voxel_text + "/" + std::to_string(stride) + "/" +
         std::to_string(max_points);
}

std::variant<Transform, std::string> parse(const nlohmann::json & json)
{
  if (!json.is_object() || !json.contains("kind") || !json["kind"].is_string()) {
    return std::string("A transform is an object with a kind");
  }
  const auto positive_integer = [&json](const char * name, uint32_t & value) -> bool {
      if (!json.contains(name)) return true;
      const auto & field = json[name];
      if (!field.is_number_integer() || field.get<int64_t>() < 1) return false;
      value = field.get<uint32_t>();
      return true;
    };
  Transform transform;
  const auto kind = json["kind"].get<std::string>();
  if (kind == "jpeg") {
    transform.kind = Transform::Kind::jpeg;
    if (json.contains("quality")) {
      const auto & quality = json["quality"];
      if (!quality.is_number_integer() || quality.get<int>() < 1 || quality.get<int>() > 100) {
        return std::string("jpeg quality is a whole number from 1 to 100");
      }
      transform.quality = quality.get<int>();
    }
    if (!positive_integer("maxWidth", transform.max_width)) {
      return std::string("jpeg maxWidth is a whole number of pixels, at least 1");
    }
    return transform;
  }
  if (kind == "reduce") {
    transform.kind = Transform::Kind::reduce;
    if (json.contains("fields")) {
      if (!json["fields"].is_array()) return std::string("reduce fields is a list of names");
      for (const auto & field : json["fields"]) {
        if (!field.is_string()) return std::string("reduce fields is a list of names");
        transform.fields.push_back(field.get<std::string>());
      }
    }
    if (json.contains("voxel")) {
      if (!json["voxel"].is_number() || json["voxel"].get<double>() <= 0) {
        return std::string("reduce voxel is a size in metres, greater than 0");
      }
      transform.voxel = json["voxel"].get<double>();
    }
    if (!positive_integer("stride", transform.stride)) {
      return std::string("reduce stride is a whole number, at least 1");
    }
    if (!positive_integer("maxPoints", transform.max_points)) {
      return std::string("reduce maxPoints is a whole number, at least 1");
    }
    return transform;
  }
  return "Unknown transform " + kind;
}

const char * input_type(Transform::Kind kind)
{
  return kind == Transform::Kind::jpeg ? "sensor_msgs/msg/Image" : "sensor_msgs/msg/PointCloud2";
}

std::optional<MessageDefinition> output_definition(const Transform & transform)
{
  if (transform.kind == Transform::Kind::jpeg) {
    return MessageDefinition{"cdr", "sensor_msgs/msg/CompressedImage", "ros2msg", COMPRESSED_IMAGE};
  }
  return std::nullopt;
}

Result apply(const Transform & transform, const Bytes & input)
{
  try {
    return transform.kind == Transform::Kind::jpeg ? jpeg(transform, input) :
           reduce(transform, input);
  } catch (const std::exception & error) {
    return std::string("Cannot read the message: ") + error.what();
  }
}

}  // namespace miti::transforms
