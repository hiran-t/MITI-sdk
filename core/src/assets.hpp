// Resolving and reading the files clients fetch (protocol.md, Asset URIs).

#pragma once

#include <string>

#include "miti/server.hpp"

namespace miti::assets
{

struct Result
{
  bool ok = false;
  std::string error;
  Bytes data;
};

inline bool enabled(const AssetOptions & options)
{
  return !options.packages.empty() || !options.roots.empty();
}

/// Reads `package://` and `file://` URIs that resolve, symlinks followed, inside a package or a
/// root, have an allowed extension and are no larger than the limit.
Result fetch(const AssetOptions & options, const std::string & uri);

}  // namespace miti::assets
