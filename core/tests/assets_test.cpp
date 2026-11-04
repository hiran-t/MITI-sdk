#include <gtest/gtest.h>

#include <fstream>
#include <random>

#include "assets.hpp"

namespace fs = std::filesystem;
using miti::AssetOptions;
using miti::assets::fetch;

namespace
{

/// The file:// URI of an absolute path: file:///home/a.stl, or file:///C:/Users/a.stl on Windows.
std::string file_uri(const fs::path & path)
{
#ifdef _WIN32
  return "file:///" + path.generic_u8string();
#else
  return "file://" + path.generic_u8string();
#endif
}

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
    // Windows makes symlinks only with Developer Mode on or as an administrator.
    std::error_code error;
    fs::create_symlink(root / "outside/secret.stl", root / "pkg/meshes/link.stl", error);
    symlinks = !error;
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

  /// Refused by the rules, not because the file is missing.
  bool not_allowed(const std::string & uri)
  {
    const auto result = fetch(options, uri);
    return !result.ok && result.error.find("not allowed") != std::string::npos;
  }

  fs::path root;
  AssetOptions options;
  bool symlinks = false;
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
  EXPECT_EQ(text(file_uri(root / "pkg/meshes/part.stl")), "solid part");
}

TEST_F(Assets, RefusesWhatLeadsOutside)
{
  EXPECT_TRUE(refused("package://pkg/../outside/secret.stl"));
  EXPECT_TRUE(refused("package://pkg/meshes/../../outside/secret.stl"));
  EXPECT_TRUE(refused(file_uri(root / "outside/secret.stl")));  // no roots
  options.roots.push_back(root / "pkg");
  EXPECT_TRUE(refused(file_uri(root / "outside/secret.stl")));
  EXPECT_TRUE(refused(file_uri(root / "pkg/meshes/link.stl")));  // a link out of a root
  EXPECT_TRUE(refused("file://relative/part.stl"));
}

TEST_F(Assets, FollowsSymlinksInsideAPackage)
{
  if (!symlinks) GTEST_SKIP() << "This user cannot make symlinks";
  // As `colcon build --symlink-install` installs a package: its files link into the source tree.
  EXPECT_EQ(text("package://pkg/meshes/link.stl"), "solid secret");
  fs::create_directories(root / "install/share");
  fs::create_directory_symlink(root / "pkg", root / "install/share/linked");
  options.packages["linked"] = root / "install/share/linked";
  EXPECT_EQ(text("package://linked/meshes/part.stl"), "solid part");
  EXPECT_TRUE(refused("package://linked/../../outside/secret.stl"));
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

#ifdef _WIN32
TEST_F(Assets, ReadsDrivePathsOnWindows)
{
  options.roots.push_back(root / "pkg");
  const auto part = root / "pkg/meshes/part.stl";
  EXPECT_EQ(text(file_uri(part)), "solid part");                          // file:///C:/dir/part.stl
  EXPECT_EQ(text("file:///" + part.u8string()), "solid part");            // file:///C:\dir\part.stl
  EXPECT_EQ(text("file://" + part.generic_u8string()), "solid part");     // file://C:/dir/part.stl
  EXPECT_TRUE(not_allowed("package://pkg/..\\outside\\secret.stl"));
}

TEST_F(Assets, RefusesPathsThatAreNotOnADriveOnWindows)
{
  // Only a drive roots a path here: \\?\C:\dir and \\host\share are refused before any lookup.
  options.roots.push_back(root / "pkg");
  const auto part = fs::canonical(root / "pkg/meshes/part.stl");
  EXPECT_TRUE(not_allowed("file://\\\\?\\" + part.u8string()));
  const auto unc = "//localhost/" + part.generic_u8string().replace(1, 1, "$");  // C:/ → C$/
  EXPECT_TRUE(not_allowed("file://" + unc));
  EXPECT_TRUE(not_allowed("file:///" + unc));
}

TEST_F(Assets, JudgesAFileByItsLongNameOnWindows)
{
  // model.stl_old is not a mesh, though its 8.3 name MODEL~1.STL looks like one.
  write("pkg/meshes/model.stl_old", "solid old");
  const auto short_name = root / "pkg/meshes/MODEL~1.STL";
  if (!fs::exists(short_name)) GTEST_SKIP() << "This volume makes no 8.3 names";
  EXPECT_TRUE(not_allowed("package://pkg/meshes/MODEL~1.STL"));
  options.roots.push_back(root / "pkg");
  EXPECT_TRUE(not_allowed(file_uri(short_name)));
}
#endif
