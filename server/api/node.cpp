#include "api/node.h"

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>

namespace eos::api {

void Node::put(const std::string& key, Value v) {
  for (auto& [k, existing] : attrs_)
    if (k == key) {
      existing = std::move(v);
      return;
    }
  attrs_.emplace_back(key, std::move(v));
}

const Value* Node::get(const std::string& key) const {
  for (const auto& [k, v] : attrs_)
    if (k == key) return &v;
  return nullptr;
}

Node& Node::child(Node n) {
  auto name = n.name();
  groups_.push_back({name, {std::move(n)}, false, false});
  return *this;
}

Node& Node::list(const std::string& name, std::vector<Node> items, bool keep_empty) {
  groups_.push_back({name, std::move(items), true, keep_empty});
  return *this;
}

Node& Node::scalars(const std::string& name, std::vector<Value> values) {
  scalar_lists_.push_back({name, std::move(values)});
  return *this;
}

namespace {

using json = nlohmann::ordered_json;

json value_json(const Value& v) {
  return std::visit([](const auto& x) { return json(x); }, v);
}

json node_json(const Node& n) {
  json j = json::object();
  for (const auto& [k, v] : n.attrs()) j[k] = value_json(v);
  if (n.text_value()) j["value"] = *n.text_value();
  for (const auto& s : n.scalar_lists()) {
    json arr = json::array();
    for (const auto& v : s.values) arr.push_back(value_json(v));
    j[s.name] = std::move(arr);
  }
  for (const auto& g : n.groups()) {
    if (g.is_list) {
      if (g.items.empty() && !g.keep_empty) continue;
      json arr = json::array();
      for (const auto& c : g.items) arr.push_back(node_json(c));
      j[g.name] = std::move(arr);
    } else {
      j[g.name] = node_json(g.items.front());
    }
  }
  return j;
}

void escape_xml(std::string& out, const std::string& s) {
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:
        // Control characters are not allowed in XML 1.0.
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
        out.push_back(c);
    }
  }
}

std::string value_text(const Value& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) return x;
        else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else if constexpr (std::is_same_v<T, double>) return std::format("{}", x);
        else return std::to_string(x);
      },
      v);
}

void node_xml(std::string& out, const Node& n, bool root) {
  out += '<';
  out += n.name();
  if (root) out += " xmlns=\"http://subsonic.org/restapi\"";
  for (const auto& [k, v] : n.attrs()) {
    out += ' ';
    out += k;
    out += "=\"";
    escape_xml(out, value_text(v));
    out += '"';
  }
  bool empty = !n.text_value() && n.groups().empty() && n.scalar_lists().empty();
  if (empty) {
    out += "/>";
    return;
  }
  out += '>';
  if (n.text_value()) escape_xml(out, *n.text_value());
  for (const auto& s : n.scalar_lists()) {
    for (const auto& v : s.values) {
      out += '<' + s.name + '>';
      escape_xml(out, value_text(v));
      out += "</" + s.name + '>';
    }
  }
  for (const auto& g : n.groups())
    for (const auto& c : g.items) node_xml(out, c, false);
  out += "</" + n.name() + '>';
}

}  // namespace

std::string to_json(const Node& root) {
  json j;
  j[root.name()] = node_json(root);
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string to_xml(const Node& root) {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  node_xml(out, root, true);
  return out;
}

std::string iso_time(int64_t unix_ms) {
  auto tp = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(unix_ms));
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z", tp);
}

}  // namespace eos::api
