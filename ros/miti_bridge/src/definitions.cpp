#include "definitions.hpp"

#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <ament_index_cpp/get_package_share_directory.hpp>

namespace miti_bridge
{

namespace
{

const std::string SEPARATOR = "\n" + std::string(80, '=') + "\n";

const std::set<std::string> PRIMITIVES{
  "bool", "byte", "char", "float32", "float64", "int8", "uint8", "int16", "uint16", "int32",
  "uint32", "int64", "uint64", "string", "wstring"};

struct TypeName
{
  std::string package, kind, name;
};

TypeName split(const std::string & type)
{
  const auto first = type.find('/');
  const auto last = type.rfind('/');
  if (first == std::string::npos || first == 0 || last + 1 == type.size()) {
    throw std::runtime_error("Not a type name: " + type);
  }
  return {
    type.substr(0, first), first == last ? "msg" : type.substr(first + 1, last - first - 1),
    type.substr(last + 1)};
}

std::string trim(const std::string & text)
{
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

/// The types a definition's fields use, as "pkg/Type", in order.
std::vector<std::string> used_types(const std::string & text, const std::string & package)
{
  std::vector<std::string> types;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line); ) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    // The type is the first word, without an array suffix or a string bound.
    std::string type = line.substr(0, line.find_first_of(" \t"));
    type = type.substr(0, type.find('['));
    type = type.substr(0, type.find("<="));
    if (PRIMITIVES.count(type)) continue;
    if (type.find('/') == std::string::npos) {
      types.push_back(package + "/" + type);
    } else {
      const auto parts = split(type);
      types.push_back(parts.package + "/" + parts.name);
    }
  }
  return types;
}

}  // namespace

std::string full_type_name(const std::string & type)
{
  const auto parts = split(type);
  return parts.package + "/" + parts.kind + "/" + parts.name;
}

std::string Definitions::read(
  const std::string & package, const std::string & kind, const std::string & name)
{
  std::string share;
  try {
    share = ament_index_cpp::get_package_share_directory(package);
  } catch (const std::exception &) {
    throw std::runtime_error("Package " + package + " is not installed");
  }
  const auto path = share + "/" + kind + "/" + name + "." + kind;
  std::ifstream file(path);
  if (!file) throw std::runtime_error("No " + path + " (a type defined only in .idl?)");
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::string Definitions::with_dependencies(const std::string & text, const std::string & package)
{
  std::string out = trim(text);
  std::set<std::string> seen;
  std::function<void(const std::string &, const std::string &)> visit =
    [&](const std::string & definition, const std::string & owner) {
      for (const auto & type : used_types(definition, owner)) {
        if (!seen.insert(type).second) continue;
        auto & dependency = texts_[type];
        if (dependency.empty()) {
          const auto parts = split(type);
          dependency = read(parts.package, "msg", parts.name);
        }
        out += SEPARATOR + "MSG: " + type + "\n" + trim(dependency);
        visit(dependency, split(type).package);
      }
    };
  visit(text, package);
  return out;
}

const miti::MessageDefinition & Definitions::message(const std::string & type)
{
  const auto name = full_type_name(type);
  const auto cached = messages_.find(name);
  if (cached != messages_.end()) return cached->second;
  const auto parts = split(name);
  if (parts.kind != "msg") throw std::runtime_error("Not a message type: " + type);
  const auto text = read(parts.package, "msg", parts.name);
  return messages_[name] = {"cdr", name, "ros2msg", with_dependencies(text, parts.package)};
}

std::pair<miti::MessageDefinition, miti::MessageDefinition> Definitions::service(
  const std::string & type)
{
  const auto parts = split(type);
  if (parts.kind != "srv") throw std::runtime_error("Not a service type: " + type);
  const auto text = read(parts.package, "srv", parts.name);
  std::string request, response;
  bool past_separator = false;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line); ) {
    if (!past_separator && trim(line) == "---") {
      past_separator = true;
      continue;
    }
    (past_separator ? response : request) += line + "\n";
  }
  if (!past_separator) throw std::runtime_error("No --- in the definition of " + type);
  const auto base = parts.package + "/srv/" + parts.name;
  return {
    {"cdr", base + "_Request", "ros2msg", with_dependencies(request, parts.package)},
    {"cdr", base + "_Response", "ros2msg", with_dependencies(response, parts.package)}};
}

}  // namespace miti_bridge
