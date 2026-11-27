#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eos {

std::string trim(std::string_view s);
std::string to_lower_ascii(std::string_view s);
std::string to_upper_ascii(std::string_view s);
bool iequals_ascii(std::string_view a, std::string_view b);
bool starts_with_ci(std::string_view s, std::string_view prefix);

// Splits `s` at any of `separators`. Empty pieces (after trimming) are dropped.
// With no separators the whole string is returned as one piece.
std::vector<std::string> split_any(std::string_view s, const std::vector<std::string>& separators);

std::string join(const std::vector<std::string>& parts, std::string_view sep);

// Parses the leading integer of strings like "3", "03", "3/12". Returns nullopt otherwise.
std::optional<int> parse_leading_int(std::string_view s);

// Parses durations like "30s", "15m", "1h", "2d". A bare number means seconds.
std::optional<long long> parse_duration_seconds(std::string_view s);

}  // namespace eos
