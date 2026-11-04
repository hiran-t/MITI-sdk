#include "assets.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <type_traits>

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

/// The extension, lowercase. One with a character beyond ASCII is no asset type's: it is empty.
std::string extension(const fs::path & path)
{
  using Unsigned = std::make_unsigned_t<fs::path::value_type>;
  const auto dotted = path.extension();
  std::string out;
  for (const auto c : dotted.native()) {
    const auto code = static_cast<Unsigned>(c);
    if (code > 127) return {};
    out += static_cast<char>(code >= 'A' && code <= 'Z' ? code - 'A' + 'a' : code);
  }
  return out;
}

/// The file a `file://` URI names, from the UTF-8 text after `file://`, if it is an absolute path.
/// On Windows `file:///C:/dir/a.stl` names C:\dir\a.stl, and a path whose root is not a drive,
/// such as \\host\share\a.stl, names none: even looking for it would reach out to that host.
std::optional<fs::path> local_path(std::string text)
{
#ifdef _WIN32
  if (text.size() > 2 && text[0] == '/' && text[2] == ':') text.erase(0, 1);
#endif
  const auto path = fs::u8path(text);
  if (!path.is_absolute()) return std::nullopt;
#ifdef _WIN32
  const auto root = path.root_name();
  if (root.native().size() != 2 || root.native()[1] != L':') return std::nullopt;
#endif
  return path;
}

}  // namespace

Result fetch(const AssetOptions & options, const std::string & uri)
{
  const auto not_allowed = [&uri] {return failure("Asset URI not allowed: " + uri);};
  const auto & allowed = options.extensions;
  const auto allowed_extension = [&allowed](const fs::path & path) {
      return std::find(allowed.begin(), allowed.end(), extension(path)) != allowed.end();
    };
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
    real = (base / fs::u8path(rest.substr(slash + 1))).lexically_normal();
    if (!inside(real, base) || !allowed_extension(real)) return not_allowed();
    if (!fs::is_regular_file(real, error)) return failure("Asset not found: " + uri);
  } else if (uri.rfind(FILE, 0) == 0) {
    const auto candidate = local_path(uri.substr(FILE.size()));
    if (!candidate || !allowed_extension(*candidate)) return not_allowed();
    // A root's files must really be inside it: symlinks and `..` are resolved before the check.
    real = fs::canonical(*candidate, error);
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
#ifdef _WIN32
  // A file also has a short 8.3 name on many Windows volumes, such as MODEL~1.STL for
  // model.stl_old: its real, long name must have an allowed extension too.
  const auto long_name = fs::canonical(real, error);
  if (error || !allowed_extension(long_name)) return not_allowed();
#endif

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
