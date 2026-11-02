// Message and service definitions from the ament index, as protocol.md's ros2msg schemas: the
// type's own definition, then each type it uses after a separator line and "MSG: pkg/Type".

#pragma once

#include <map>
#include <string>
#include <utility>

#include <miti/server.hpp>

namespace miti_bridge
{

/// `pkg/Type` → `pkg/msg/Type`; names that already have a middle part stay as they are.
std::string full_type_name(const std::string & type);

class Definitions
{
public:
  /// The definition of `pkg/msg/Type` (or `pkg/Type`). Throws std::runtime_error when it, or a
  /// type it uses, has no .msg file: types that ship only as .idl, or packages not installed.
  const miti::MessageDefinition & message(const std::string & type);

  /// The request and response of `pkg/srv/Type`, from its .srv file.
  std::pair<miti::MessageDefinition, miti::MessageDefinition> service(const std::string & type);

private:
  /// The .msg or .srv text of `pkg/<kind>/Type`.
  static std::string read(const std::string & package, const std::string & kind,
    const std::string & name);
  /// `text` followed by the definitions of every type it uses, depth first, each once.
  std::string with_dependencies(const std::string & text, const std::string & package);

  std::map<std::string, miti::MessageDefinition> messages_;
  std::map<std::string, std::string> texts_;  // "pkg/Type" → its .msg text
};

}  // namespace miti_bridge
