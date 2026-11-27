#include "api/subsonic/context.h"

#include <charconv>

#include "core/log.h"
#include "core/strings.h"

namespace eos::api {

Services::Services(const Config& c, db::ReadPool& r, db::WriteQueue& w, scanner::Scanner& s,
                   crypto::Bytes key)
    : cfg(c),
      reads(r),
      writer(w),
      scanner(s),
      secret_key(std::move(key)),
      trusted_proxies(c.trusted_proxies),
      rate_limit_exempt(c.rate_limit_exempt),
      artwork(c),
      transcoder(c) {}

Services::~Services() {
  if (scan_thread_.joinable()) scan_thread_.join();
}

void Services::touch_api_key(const std::string& key_id) {
  auto now = db::now_ms();
  {
    std::lock_guard lock(mutex_);
    auto& last = key_touched_[key_id];
    if (now - last < 5 * 60 * 1000) return;
    last = now;
  }
  // Fire and forget; the future is dropped on purpose.
  (void)writer.submit([key_id, now](db::Connection& c) {
    c.run("UPDATE api_key SET last_used_at = ? WHERE id = ?", now, key_id);
  });
}

bool Services::start_scan(bool full) {
  std::lock_guard lock(mutex_);
  if (scan_running_.load() || scanner.status().scanning) return false;
  if (scan_thread_.joinable()) scan_thread_.join();
  scan_running_ = true;
  scan_thread_ = std::thread([this, full] {
    try {
      scanner.scan(full);
    } catch (const std::exception& e) {
      log::error("scan failed: {}", e.what());
    }
    scan_running_ = false;
  });
  return true;
}

std::string Ctx::require(std::string_view name) const {
  auto v = req.param(name);
  if (!v) throw SubsonicError(err::missing_param, std::format("Required parameter is missing: {}", name));
  return *v;
}

std::optional<int64_t> Ctx::opt_int(std::string_view name) const {
  auto v = req.param(name);
  if (!v) return std::nullopt;
  auto s = trim(*v);
  int64_t out = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  if (ec != std::errc() || p != s.data() + s.size())
    throw SubsonicError(err::generic, std::format("Parameter {} must be an integer", name));
  return out;
}

int64_t Ctx::int_param(std::string_view name, int64_t def, int64_t min, int64_t max) const {
  auto v = opt_int(name).value_or(def);
  return std::clamp(v, min, max);
}

bool Ctx::bool_param(std::string_view name, bool def) const {
  auto v = req.param(name);
  if (!v) return def;
  auto s = to_lower_ascii(trim(*v));
  return s == "true" || s == "1" || s == "yes";
}

void Ctx::require_admin() const {
  if (!user.admin) throw SubsonicError(err::not_authorized, "User is not authorized for this operation");
}

}  // namespace eos::api
