#pragma once

#include <format>
#include <string>
#include <string_view>

namespace eos::log {

enum class Level { debug = 0, info = 1, warn = 2, error = 3 };

void set_level(Level level);
bool set_level(std::string_view name);  // false if the name is unknown
Level level();

// Writes one line to stderr. The message passes through redact() first, so a URL
// that slipped into a log line never leaks credentials.
void write(Level level, std::string_view message);

// Replaces the values of the credential parameters t, s, p and apiKey in a URL or
// query string with "***".
std::string redact(std::string_view text);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::debug) write(Level::debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::info) write(Level::info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::warn) write(Level::warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  write(Level::error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace eos::log
