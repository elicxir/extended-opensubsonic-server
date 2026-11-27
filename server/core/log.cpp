#include "core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "core/strings.h"

namespace eos::log {
namespace {
std::atomic<Level> g_level{Level::info};
std::mutex g_mutex;

const char* level_name(Level l) {
  switch (l) {
    case Level::debug: return "DEBUG";
    case Level::info: return "INFO";
    case Level::warn: return "WARN";
    case Level::error: return "ERROR";
  }
  return "?";
}

bool is_secret_param(std::string_view name) {
  return name == "t" || name == "s" || name == "p" || iequals_ascii(name, "apikey");
}
}  // namespace

void set_level(Level l) { g_level.store(l); }

bool set_level(std::string_view name) {
  auto n = to_lower_ascii(name);
  if (n == "debug") set_level(Level::debug);
  else if (n == "info") set_level(Level::info);
  else if (n == "warn" || n == "warning") set_level(Level::warn);
  else if (n == "error") set_level(Level::error);
  else return false;
  return true;
}

Level level() { return g_level.load(); }

std::string redact(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    // A parameter name starts after '?' or '&' (or at the very start of a bare query).
    bool at_param = (i == 0) || text[i - 1] == '?' || text[i - 1] == '&';
    if (at_param) {
      size_t eq = text.find('=', i);
      size_t amp = text.find_first_of("& \t\r\n", i);
      if (eq != std::string_view::npos && (amp == std::string_view::npos || eq < amp)) {
        auto name = text.substr(i, eq - i);
        if (is_secret_param(name)) {
          out.append(name);
          out.append("=***");
          size_t end = text.find_first_of("& \t\r\n", eq + 1);
          i = (end == std::string_view::npos) ? text.size() : end;
          continue;
        }
      }
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

void write(Level l, std::string_view message) {
  if (l < level()) return;
  auto now = std::chrono::system_clock::now();
  auto line = std::format("{:%Y-%m-%dT%H:%M:%S}Z {} {}\n",
                          std::chrono::floor<std::chrono::seconds>(now), level_name(l),
                          redact(message));
  std::lock_guard lock(g_mutex);
  std::fputs(line.c_str(), stderr);
  std::fflush(stderr);
}

}  // namespace eos::log
