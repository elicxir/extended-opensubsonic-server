#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace eos::api {

using Value = std::variant<std::string, int64_t, double, bool>;

// A response element that serialises to both Subsonic XML and JSON:
//   scalars  -> XML attributes / JSON fields
//   text     -> XML text content / JSON "value"
//   children -> XML child elements / JSON object (single) or array (list)
//   scalar lists -> repeated XML elements with text / JSON array of scalars
class Node {
 public:
  explicit Node(std::string name) : name_(std::move(name)) {}

  template <class T>
  Node& set(const std::string& key, const T& v) {
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, Value>) {
      put(key, v);
    } else if constexpr (std::is_same_v<U, bool>) {
      put(key, Value(v));
    } else if constexpr (std::is_integral_v<U>) {
      put(key, Value(static_cast<int64_t>(v)));
    } else if constexpr (std::is_floating_point_v<U>) {
      put(key, Value(static_cast<double>(v)));
    } else {
      put(key, Value(std::string(v)));
    }
    return *this;
  }
  template <class T>
  Node& set_opt(const std::string& key, const std::optional<T>& v) {
    if (v) set(key, *v);
    return *this;
  }
  // Skips empty strings.
  Node& set_nonempty(const std::string& key, const std::string& v) {
    if (!v.empty()) set(key, v);
    return *this;
  }

  Node& text(std::string t) {
    text_ = std::move(t);
    return *this;
  }

  // A single child element (JSON object).
  Node& child(Node n);
  // A list of child elements with the same name (JSON array). With keep_empty the
  // JSON array is written even when empty.
  Node& list(const std::string& name, std::vector<Node> items, bool keep_empty = false);
  Node& scalars(const std::string& name, std::vector<Value> values);

  const std::string& name() const { return name_; }
  const std::vector<std::pair<std::string, Value>>& attrs() const { return attrs_; }
  const std::optional<std::string>& text_value() const { return text_; }

  struct Group {
    std::string name;
    std::vector<Node> items;
    bool is_list = false;
    bool keep_empty = false;
  };
  struct ScalarList {
    std::string name;
    std::vector<Value> values;
  };
  const std::vector<Group>& groups() const { return groups_; }
  const std::vector<ScalarList>& scalar_lists() const { return scalar_lists_; }

  const Value* get(const std::string& key) const;

 private:
  void put(const std::string& key, Value v);

  std::string name_;
  std::vector<std::pair<std::string, Value>> attrs_;
  std::optional<std::string> text_;
  std::vector<Group> groups_;
  std::vector<ScalarList> scalar_lists_;
};

// `root` is the subsonic-response element.
std::string to_json(const Node& root);
std::string to_xml(const Node& root);

std::string iso_time(int64_t unix_ms);

}  // namespace eos::api
