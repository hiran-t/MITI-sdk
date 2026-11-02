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

/// Whether `path` is `base` or inside it, both already canonical.
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
  fs::path candidate;
  std::vector<fs::path> bases;
  if (uri.rfind(PACKAGE, 0) == 0) {
    const auto rest = uri.substr(PACKAGE.size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos) return not_allowed();
    const auto package = options.packages.find(rest.substr(0, slash));
    if (package == options.packages.end()) return not_allowed();
    candidate = package->second / rest.substr(slash + 1);
    bases.push_back(package->second);
  } else if (uri.rfind(FILE, 0) == 0) {
    candidate = uri.substr(FILE.size());
    if (!candidate.is_absolute()) return not_allowed();
    bases = options.roots;
  } else {
    return not_allowed();
  }

  const auto & allowed = options.extensions;
  if (std::find(allowed.begin(), allowed.end(), lowercase(candidate.extension().string())) ==
    allowed.end())
  {
    return not_allowed();
  }

  // Symlinks and `..` are resolved before the check, so neither can lead outside.
  std::error_code error;
  const auto real = fs::canonical(candidate, error);
  if (error || !fs::is_regular_file(real, error)) return failure("Asset not found: " + uri);
  const bool within = std::any_of(
    bases.begin(), bases.end(), [&real](const fs::path & base) {
      std::error_code base_error;
      const auto real_base = fs::canonical(base, base_error);
      return !base_error && inside(real, real_base);
    });
  if (!within) return not_allowed();

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
