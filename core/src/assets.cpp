#include "assets.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace miti::assets
{

namespace
{

constexpr std::string_view PACKAGE = "package://";
constexpr std::string_view FILE = "file://";

Result failure(std::string error)
{
  return {false, std::move(error), {}};
}

/// Whether `path` is `base` or inside it, both canonical or both lexically normal.
bool inside(const fs::path & path, const fs::path & base)
{
  const auto relative = path.lexically_relative(base);
  return !relative.empty() && *relative.begin() != "..";
}

std::string lowercase(std::string text)
{
  std::transform(
    text.begin(), text.end(), text.begin(), [](unsigned char c) {return std::tolower(c);});
  return text;
}

}  // namespace

Result fetch(const AssetOptions & options, const std::string & uri)
{
  const auto not_allowed = [&uri] {return failure("Asset URI not allowed: " + uri);};
  const auto & allowed = options.extensions;
  const auto allowed_extension = [&allowed](const fs::path & path) {
      return std::find(allowed.begin(), allowed.end(), lowercase(path.extension().string())) !=
             allowed.end();
    };
  // The extension is checked on the URI as written, but the file system stops at the first NUL.
  if (uri.find('\0') != std::string::npos) return not_allowed();
  std::error_code error;
  fs::path real;
  if (uri.rfind(PACKAGE, 0) == 0) {
    const auto rest = uri.substr(PACKAGE.size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos) return not_allowed();
    const auto package = options.packages.find(rest.substr(0, slash));
    if (package == options.packages.end()) return not_allowed();
    // `..` cannot lead out of the package, checked on the path as written. Symlinks inside it are
    // followed: `colcon build --symlink-install` installs every file as one, into the source tree.
    const auto base = package->second.lexically_normal();
    real = (base / rest.substr(slash + 1)).lexically_normal();
    if (!inside(real, base) || !allowed_extension(real)) return not_allowed();
    if (!fs::is_regular_file(real, error)) return failure("Asset not found: " + uri);
  } else if (uri.rfind(FILE, 0) == 0) {
    const fs::path candidate = uri.substr(FILE.size());
    if (!candidate.is_absolute() || !allowed_extension(candidate)) return not_allowed();
    // A root's files must really be inside it: symlinks and `..` are resolved before the check.
    real = fs::canonical(candidate, error);
    if (error || !fs::is_regular_file(real, error)) return failure("Asset not found: " + uri);
    const bool within = std::any_of(
      options.roots.begin(), options.roots.end(), [&real](const fs::path & root) {
        std::error_code root_error;
        const auto real_root = fs::canonical(root, root_error);
        return !root_error && inside(real, real_root);
      });
    if (!within) return not_allowed();
  } else {
    return not_allowed();
  }

  const auto size = fs::file_size(real, error);
  if (error) return failure("Asset not found: " + uri);
  if (size > options.max_size) return failure("Asset is larger than the limit: " + uri);

  std::ifstream file(real, std::ios::binary);
  Result result{true, {}, Bytes(size)};
  if (!file.read(reinterpret_cast<char *>(result.data.data()), static_cast<std::streamsize>(size)))
    return failure("Cannot read asset: " + uri);
  return result;
}

}  // namespace miti::assets
