#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include <turbojpeg.h>

#include "cdr.hpp"
#include "transforms.hpp"

using json = nlohmann::json;
using miti::Bytes;
using miti::transforms::Transform;
namespace cdr = miti::cdr;
namespace transforms = miti::transforms;

namespace
{

const cdr::Header HEADER{12, 34, "camera"};

Bytes image(uint32_t width, uint32_t height, const std::string & encoding, uint32_t channels)
{
  Bytes pixels(size_t(width) * height * channels);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 7);
  cdr::Writer writer;
  cdr::write_header(writer, HEADER);
  writer.u32(height).u32(width).string(encoding).u8(0).u32(width * channels);
  writer.bytes(pixels.data(), pixels.size());
  return writer.take();
}

Transform parse(const json & spec)
{
  auto parsed = transforms::parse(spec);
  if (auto * error = std::get_if<std::string>(&parsed)) ADD_FAILURE() << *error;
  return std::get<Transform>(parsed);
}

Bytes apply(const json & spec, const Bytes & input)
{
  auto result = transforms::apply(parse(spec), input);
  if (auto * error = std::get_if<std::string>(&result)) ADD_FAILURE() << *error;
  return std::get<Bytes>(result);
}

std::string error(const json & spec, const Bytes & input)
{
  auto result = transforms::apply(parse(spec), input);
  return std::holds_alternative<std::string>(result) ? std::get<std::string>(result) : "";
}

struct Jpeg
{
  cdr::Header header;
  std::string format;
  Bytes data;
  int width = 0, height = 0, subsampling = -1;
};

Jpeg read_jpeg(const Bytes & message)
{
  cdr::Reader reader(message.data(), message.size());
  Jpeg jpeg;
  jpeg.header = cdr::read_header(reader);
  jpeg.format = reader.string();
  const auto [data, size] = reader.bytes();
  jpeg.data.assign(data, data + size);
  tjhandle handle = tjInitDecompress();
  int colorspace = 0;
  EXPECT_EQ(tjDecompressHeader3(handle, jpeg.data.data(), jpeg.data.size(), &jpeg.width,
    &jpeg.height, &jpeg.subsampling, &colorspace), 0);
  tjDestroy(handle);
  return jpeg;
}

/// A field of a point cloud: float32 is datatype 7, uint16 is 4.
struct Field
{
  std::string name;
  uint32_t offset;
  uint8_t datatype;
};

const std::vector<Field> LIDAR{{"x", 0, 7}, {"y", 4, 7}, {"z", 8, 7}, {"intensity", 16, 7},
  {"ring", 20, 4}};
constexpr uint32_t LIDAR_STEP = 32;  // with padding, as drivers often send

/// A cloud of LIDAR points, `height` rows of `points.size() / height`.
Bytes cloud(const std::vector<std::array<float, 4>> & points, uint32_t height = 1)
{
  Bytes data(points.size() * LIDAR_STEP, 0xee);
  for (size_t i = 0; i < points.size(); ++i) {
    uint8_t * point = data.data() + i * LIDAR_STEP;
    std::memcpy(point, points[i].data(), 12);
    std::memcpy(point + 16, &points[i][3], 4);
    const uint16_t ring = static_cast<uint16_t>(i);
    std::memcpy(point + 20, &ring, 2);
  }
  const uint32_t width = static_cast<uint32_t>(points.size() / height);
  cdr::Writer writer;
  cdr::write_header(writer, HEADER);
  writer.u32(height).u32(width).u32(static_cast<uint32_t>(LIDAR.size()));
  for (const auto & field : LIDAR) {
    writer.string(field.name).u32(field.offset).u8(field.datatype).u32(1);
  }
  writer.boolean(false).u32(LIDAR_STEP).u32(LIDAR_STEP * width);
  writer.bytes(data.data(), data.size()).boolean(false);
  return writer.take();
}

struct Cloud
{
  uint32_t height, width, point_step, row_step;
  std::vector<Field> fields;
  bool dense;
  Bytes data;

  float value(size_t point, const std::string & name) const
  {
    for (const auto & field : fields) {
      if (field.name == name) {
        float out;
        std::memcpy(&out, data.data() + point * point_step + field.offset, 4);
        return out;
      }
    }
    ADD_FAILURE() << "no field " << name;
    return 0;
  }
};

Cloud read_cloud(const Bytes & message)
{
  cdr::Reader reader(message.data(), message.size());
  Cloud cloud;
  EXPECT_EQ(cdr::read_header(reader).frame_id, HEADER.frame_id);
  cloud.height = reader.u32();
  cloud.width = reader.u32();
  cloud.fields.resize(reader.u32());
  for (auto & field : cloud.fields) {
    field.name = reader.string();
    field.offset = reader.u32();
    field.datatype = reader.u8();
    EXPECT_EQ(reader.u32(), 1u);
  }
  EXPECT_FALSE(reader.boolean());
  cloud.point_step = reader.u32();
  cloud.row_step = reader.u32();
  const auto [data, size] = reader.bytes();
  cloud.data.assign(data, data + size);
  cloud.dense = reader.boolean();
  return cloud;
}

std::vector<std::array<float, 4>> line(size_t count, float spacing)
{
  std::vector<std::array<float, 4>> points;
  for (size_t i = 0; i < count; ++i) points.push_back({spacing * i, 0, 0, float(i)});
  return points;
}

}  // namespace

TEST(Transforms, JpegEncodesColourImagesKeepingTheirHeader)
{
  for (const auto & [encoding, channels] :
    std::vector<std::pair<std::string, uint32_t>>{{"rgb8", 3}, {"bgr8", 3}, {"rgba8", 4},
      {"bgra8", 4}})
  {
    const auto jpeg = read_jpeg(apply({{"kind", "jpeg"}}, image(64, 48, encoding, channels)));
    EXPECT_EQ(jpeg.header.sec, HEADER.sec) << encoding;
    EXPECT_EQ(jpeg.header.nanosec, HEADER.nanosec);
    EXPECT_EQ(jpeg.header.frame_id, HEADER.frame_id);
    EXPECT_EQ(jpeg.format, "jpeg");
    ASSERT_GE(jpeg.data.size(), 2u);
    EXPECT_EQ(jpeg.data[0], 0xff);
    EXPECT_EQ(jpeg.data[1], 0xd8);
    EXPECT_EQ(jpeg.width, 64);
    EXPECT_EQ(jpeg.height, 48);
    EXPECT_EQ(jpeg.subsampling, TJSAMP_420);
  }
}

TEST(Transforms, JpegEncodesMono8AsGrayscale)
{
  const auto jpeg = read_jpeg(apply({{"kind", "jpeg"}}, image(40, 30, "mono8", 1)));
  EXPECT_EQ(jpeg.width, 40);
  EXPECT_EQ(jpeg.subsampling, TJSAMP_GRAY);
}

TEST(Transforms, JpegShrinksWiderImagesToMaxWidth)
{
  const auto shrunk = read_jpeg(apply({{"kind", "jpeg"}, {"maxWidth", 32}},
    image(64, 48, "rgb8", 3)));
  EXPECT_EQ(shrunk.width, 32);
  EXPECT_EQ(shrunk.height, 24);
  const auto kept = read_jpeg(apply({{"kind", "jpeg"}, {"maxWidth", 100}},
    image(64, 48, "rgb8", 3)));
  EXPECT_EQ(kept.width, 64);
}

TEST(Transforms, JpegQualityChangesTheSize)
{
  const auto input = image(64, 48, "rgb8", 3);
  const auto low = read_jpeg(apply({{"kind", "jpeg"}, {"quality", 10}}, input));
  const auto high = read_jpeg(apply({{"kind", "jpeg"}, {"quality", 95}}, input));
  EXPECT_LT(low.data.size(), high.data.size());
}

TEST(Transforms, JpegDropsImagesItCannotEncode)
{
  EXPECT_EQ(error({{"kind", "jpeg"}}, image(4, 4, "16UC1", 2)),
    "jpeg cannot encode 16UC1 images; their messages are dropped");
  auto shorter = image(4, 4, "rgb8", 3);
  shorter.resize(shorter.size() - 1);
  EXPECT_NE(error({{"kind", "jpeg"}}, shorter), "");
  EXPECT_EQ(error({{"kind", "jpeg"}}, image(0, 0, "rgb8", 3)), "The image is empty");
}

TEST(Transforms, ReduceKeepsTheFieldsAskedForAndAlwaysXyz)
{
  const auto points = line(3, 1);
  const auto reduced = read_cloud(apply({{"kind", "reduce"}, {"fields", {"intensity"}}},
    cloud(points)));
  ASSERT_EQ(reduced.fields.size(), 4u);
  EXPECT_EQ(reduced.fields[3].name, "intensity");
  EXPECT_EQ(reduced.fields[3].offset, 12u);
  EXPECT_EQ(reduced.point_step, 16u);  // repacked without padding
  EXPECT_EQ(reduced.height, 1u);
  EXPECT_EQ(reduced.width, 3u);
  EXPECT_EQ(reduced.row_step, 48u);
  EXPECT_EQ(reduced.data.size(), 48u);
  EXPECT_TRUE(reduced.dense);
  for (size_t i = 0; i < points.size(); ++i) {
    EXPECT_EQ(reduced.value(i, "x"), points[i][0]);
    EXPECT_EQ(reduced.value(i, "intensity"), points[i][3]);
  }

  const auto all = read_cloud(apply({{"kind", "reduce"}}, cloud(points)));
  EXPECT_EQ(all.fields.size(), LIDAR.size());
  EXPECT_EQ(all.point_step, 18u);  // four floats and a uint16
}

TEST(Transforms, ReduceSkipsPointsThatAreNotFinite)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  // Two rows, as an organised cloud from a depth camera.
  const auto reduced = read_cloud(apply({{"kind", "reduce"}},
    cloud({{0, 0, 0, 1}, {nan, 0, 0, 2}, {0, inf, 0, 3}, {1, 1, 1, 4}}, 2)));
  EXPECT_EQ(reduced.height, 1u);
  ASSERT_EQ(reduced.width, 2u);
  EXPECT_EQ(reduced.value(0, "intensity"), 1);
  EXPECT_EQ(reduced.value(1, "intensity"), 4);
}

TEST(Transforms, ReduceKeepsTheFirstPointInEachVoxel)
{
  // 0, 0.03, 0.06 ... 0.27: 0.1 m voxels hold the points at 0, 0.12 and 0.21 first.
  const auto reduced = read_cloud(apply({{"kind", "reduce"}, {"voxel", 0.1}},
    cloud(line(10, 0.03f))));
  ASSERT_EQ(reduced.width, 3u);
  EXPECT_EQ(reduced.value(0, "intensity"), 0);
  EXPECT_EQ(reduced.value(1, "intensity"), 4);
  EXPECT_EQ(reduced.value(2, "intensity"), 7);
}

TEST(Transforms, ReduceThinsByStrideThenToMaxPoints)
{
  const auto strided = read_cloud(apply({{"kind", "reduce"}, {"stride", 3}},
    cloud(line(10, 1))));
  ASSERT_EQ(strided.width, 4u);
  EXPECT_EQ(strided.value(3, "intensity"), 9);

  const auto even = read_cloud(apply({{"kind", "reduce"}, {"maxPoints", 4}},
    cloud(line(100, 1))));
  ASSERT_EQ(even.width, 4u);
  EXPECT_EQ(even.value(0, "intensity"), 0);
  EXPECT_EQ(even.value(1, "intensity"), 25);
  EXPECT_EQ(even.value(3, "intensity"), 75);
}

TEST(Transforms, ReduceNeedsFloatCoordinates)
{
  cdr::Writer writer;
  cdr::write_header(writer, HEADER);
  writer.u32(1).u32(1).u32(1).string("x").u32(0).u8(7).u32(1);
  writer.boolean(false).u32(4).u32(4).bytes(Bytes(4).data(), 4).boolean(true);
  EXPECT_EQ(error({{"kind", "reduce"}}, writer.take()), "reduce needs float x, y and z fields");
  EXPECT_EQ(error({{"kind", "reduce"}}, Bytes{0, 1, 0, 0, 1}).rfind("Cannot read", 0), 0u);
}

TEST(Transforms, ParseChecksEveryOption)
{
  const auto refused = [](const json & spec) {
      return std::holds_alternative<std::string>(transforms::parse(spec));
    };
  EXPECT_TRUE(refused(json::array()));
  EXPECT_TRUE(refused({{"kind", "png"}}));
  EXPECT_TRUE(refused({{"kind", "jpeg"}, {"quality", 0}}));
  EXPECT_TRUE(refused({{"kind", "jpeg"}, {"quality", 101}}));
  EXPECT_TRUE(refused({{"kind", "jpeg"}, {"quality", 80.5}}));
  EXPECT_TRUE(refused({{"kind", "jpeg"}, {"maxWidth", 0}}));
  EXPECT_TRUE(refused({{"kind", "reduce"}, {"fields", "rgb"}}));
  EXPECT_TRUE(refused({{"kind", "reduce"}, {"fields", {1}}}));
  EXPECT_TRUE(refused({{"kind", "reduce"}, {"voxel", 0}}));
  EXPECT_TRUE(refused({{"kind", "reduce"}, {"stride", -1}}));
  EXPECT_TRUE(refused({{"kind", "reduce"}, {"maxPoints", "many"}}));

  const auto jpeg = parse({{"kind", "jpeg"}});
  EXPECT_EQ(jpeg.quality, 80);
  EXPECT_STREQ(transforms::input_type(jpeg.kind), "sensor_msgs/msg/Image");
  EXPECT_EQ(transforms::output_definition(jpeg)->schema_name, "sensor_msgs/msg/CompressedImage");
  const auto reduce = parse({{"kind", "reduce"}, {"voxel", 0.05}});
  EXPECT_STREQ(transforms::input_type(reduce.kind), "sensor_msgs/msg/PointCloud2");
  EXPECT_FALSE(transforms::output_definition(reduce));
}

TEST(Transforms, KeysMatchOnlyTransformsThatMakeTheSameOutput)
{
  EXPECT_EQ(parse({{"kind", "jpeg"}}).key(), parse({{"kind", "jpeg"}, {"quality", 80}}).key());
  EXPECT_NE(parse({{"kind", "jpeg"}}).key(), parse({{"kind", "jpeg"}, {"maxWidth", 640}}).key());
  EXPECT_NE(parse({{"kind", "reduce"}, {"voxel", 0.1}}).key(),
    parse({{"kind", "reduce"}, {"voxel", 0.10000001}}).key());
  EXPECT_NE(parse({{"kind", "reduce"}, {"fields", {"a", "b"}}}).key(),
    parse({{"kind", "reduce"}, {"fields", {"ab"}}}).key());
}
