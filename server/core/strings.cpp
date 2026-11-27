#include "core/strings.h"

#include <algorithm>
#include <cctype>

namespace eos {

std::string trim(std::string_view s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  size_t b = 0, e = s.size();
  while (b < e && is_space(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && is_space(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::string to_lower_ascii(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string to_upper_ascii(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
  return a.size() == b.size() && to_lower_ascii(a) == to_lower_ascii(b);
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals_ascii(s.substr(0, prefix.size()), prefix);
}

std::vector<std::string> split_any(std::string_view s, const std::vector<std::string>& separators) {
  std::vector<std::string> out;
  size_t start = 0, pos = 0;
  while (pos < s.size()) {
    size_t matched = 0;
    for (const auto& sep : separators) {
      if (!sep.empty() && s.substr(pos, sep.size()) == sep) {
        matched = sep.size();
        break;
      }
    }
    if (matched > 0) {
      auto piece = trim(s.substr(start, pos - start));
      if (!piece.empty()) out.push_back(std::move(piece));
      pos += matched;
      start = pos;
    } else {
      ++pos;
    }
  }
  auto piece = trim(s.substr(start));
  if (!piece.empty()) out.push_back(std::move(piece));
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

std::optional<int> parse_leading_int(std::string_view s) {
  auto t = trim(s);
  size_t i = 0;
  int value = 0;
  while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
    if (value > 100'000'000) return std::nullopt;
    value = value * 10 + (t[i] - '0');
    ++i;
  }
  if (i == 0) return std::nullopt;
  return value;
}

std::optional<long long> parse_duration_seconds(std::string_view s) {
  auto t = trim(s);
  if (t.empty()) return std::nullopt;
  long long mult = 1;
  char unit = t.back();
  if (!std::isdigit(static_cast<unsigned char>(unit))) {
    switch (std::tolower(static_cast<unsigned char>(unit))) {
      case 's': mult = 1; break;
      case 'm': mult = 60; break;
      case 'h': mult = 3600; break;
      case 'd': mult = 86400; break;
      default: return std::nullopt;
    }
    t.pop_back();
  }
  if (t.empty() || !std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); }))
    return std::nullopt;
  return std::stoll(t) * mult;
}

}  // namespace eos
