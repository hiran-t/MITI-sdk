#include <gtest/gtest.h>

#include <fstream>
#include <random>

#include "assets.hpp"

namespace fs = std::filesystem;
using miti::AssetOptions;
using miti::assets::fetch;

namespace
{

class Assets : public ::testing::Test
{
protected:
  void SetUp() override
  {
    root = fs::temp_directory_path() / ("miti_assets_" + std::to_string(std::random_device{}()));
    write("pkg/meshes/part.stl", "solid part");
    write("pkg/meshes/PART2.STL", "solid upper");
    write("pkg/meshes/notes.txt", "not a mesh");
    write("outside/secret.stl", "solid secret");
    fs::create_symlink(root / "outside/secret.stl", root / "pkg/meshes/link.stl");
    options.packages["pkg"] = root / "pkg";
  }

  void TearDown() override {fs::remove_all(root);}

  void write(const std::string & path, const std::string & content)
  {
    fs::create_directories((root / path).parent_path());
    std::ofstream(root / path) << content;
  }

  std::string text(const std::string & uri)
  {
    const auto result = fetch(options, uri);
    EXPECT_TRUE(result.ok) << uri << ": " << result.error;
    return std::string(result.data.begin(), result.data.end());
  }

  bool refused(const std::string & uri)
  {
    const auto result = fetch(options, uri);
    return !result.ok && !result.error.empty() && result.data.empty();
  }

  fs::path root;
  AssetOptions options;
};

}  // namespace

TEST_F(Assets, ServesPackageFiles)
{
  EXPECT_EQ(text("package://pkg/meshes/part.stl"), "solid part");
  EXPECT_EQ(text("package://pkg/meshes/PART2.STL"), "solid upper");
}

TEST_F(Assets, ServesFilesInsideARoot)
{
  options.roots.push_back(root / "pkg");
  EXPECT_EQ(text("file://" + (root / "pkg/meshes/part.stl").string()), "solid part");
}

TEST_F(Assets, RefusesWhatLeadsOutside)
{
  EXPECT_TRUE(refused("package://pkg/../outside/secret.stl"));
  EXPECT_TRUE(refused("package://pkg/meshes/link.stl"));  // a symlink out of the package
  EXPECT_TRUE(refused("file://" + (root / "outside/secret.stl").string()));  // no roots
  options.roots.push_back(root / "pkg");
  EXPECT_TRUE(refused("file://" + (root / "outside/secret.stl").string()));
  EXPECT_TRUE(refused("file://relative/part.stl"));
}

TEST_F(Assets, RefusesUnknownPackagesSchemesAndExtensions)
{
  EXPECT_TRUE(refused("package://other/meshes/part.stl"));
  EXPECT_TRUE(refused("package://pkg"));
  EXPECT_TRUE(refused("http://example.com/part.stl"));
  EXPECT_TRUE(refused("package://pkg/meshes/notes.txt"));
}

TEST_F(Assets, ReportsMissingAndOversizedFiles)
{
  const auto missing = fetch(options, "package://pkg/meshes/missing.stl");
  EXPECT_FALSE(missing.ok);
  EXPECT_NE(missing.error.find("not found"), std::string::npos) << missing.error;
  options.max_size = 4;
  const auto large = fetch(options, "package://pkg/meshes/part.stl");
  EXPECT_FALSE(large.ok);
  EXPECT_NE(large.error.find("larger"), std::string::npos) << large.error;
}
