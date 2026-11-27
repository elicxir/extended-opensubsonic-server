#include "scanner/pid.h"

#include <stdexcept>

#include "core/ids.h"
#include "core/normalize.h"
#include "core/strings.h"

namespace eos::scanner {

PidSpec PidSpec::parse(std::string_view spec) {
  PidSpec out;
  for (const auto& alt : split_any(spec, {"|"})) {
    std::vector<std::string> fields;
    for (const auto& f : split_any(alt, {","})) fields.push_back(to_lower_ascii(f));
    if (!fields.empty()) out.alternatives.push_back(std::move(fields));
  }
  if (out.alternatives.empty()) throw std::invalid_argument("empty PID spec");
  return out;
}

std::string compute_pid(const PidSpec& spec, const FieldLookup& lookup) {
  const std::vector<std::string>* chosen = &spec.alternatives.back();
  std::vector<std::string> values;
  for (const auto& alt : spec.alternatives) {
    std::vector<std::string> vals;
    bool complete = true;
    for (const auto& f : alt) {
      auto v = normalize(lookup(f));
      complete = complete && !v.empty();
      vals.push_back(std::move(v));
    }
    if (complete || &alt == &spec.alternatives.back()) {
      chosen = &alt;
      values = std::move(vals);
      break;
    }
  }
  std::string key = "pid";
  for (size_t i = 0; i < chosen->size(); ++i) {
    key.push_back('\x1f');
    key += (*chosen)[i];
    key.push_back('=');
    key += values[i];
  }
  return ids::hash_id(key);
}

}  // namespace eos::scanner
