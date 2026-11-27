#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include "api/http.h"
#include "api/net.h"
#include "api/node.h"
#include "api/rate_limit.h"
#include "core/config.h"
#include "core/crypto.h"
#include "db/database.h"
#include "db/write_queue.h"
#include "media/artwork.h"
#include "media/transcode.h"
#include "scanner/scanner.h"
#include "sync/hub.h"

namespace eos::api {

inline constexpr const char* kApiVersion = "1.16.1";
inline constexpr const char* kServerType = "eos";

// Subsonic error codes (OpenSubsonic: responses/error).
namespace err {
inline constexpr int generic = 0;
inline constexpr int missing_param = 10;
inline constexpr int wrong_credentials = 40;
inline constexpr int token_not_supported = 41;
inline constexpr int auth_not_supported = 42;
inline constexpr int conflicting_auth = 43;
inline constexpr int invalid_api_key = 44;
inline constexpr int not_authorized = 50;
inline constexpr int not_found = 70;
}  // namespace err

struct SubsonicError : std::runtime_error {
  int code;
  SubsonicError(int c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

// Long-lived state shared by every request.
class Services {
 public:
  Services(const Config& cfg, db::ReadPool& reads, db::WriteQueue& writer, scanner::Scanner& scanner,
           crypto::Bytes secret_key);
  ~Services();

  const Config& cfg;
  db::ReadPool& reads;
  db::WriteQueue& writer;
  scanner::Scanner& scanner;
  const crypto::Bytes secret_key;
  RateLimiter limiter;
  const IpSet trusted_proxies;
  const IpSet rate_limit_exempt;
  media::Artwork artwork;
  media::Transcoder transcoder;
  sync::SessionHub* hub = nullptr;  // set when /ext/session is served

  // Records API key use at most once every 5 minutes per key.
  void touch_api_key(const std::string& key_id);
  // Starts a scan in the background unless one is running. Returns false if busy.
  bool start_scan(bool full);

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, int64_t> key_touched_;
  std::thread scan_thread_;
  std::atomic<bool> scan_running_{false};
};

struct AuthUser {
  int64_t id = 0;
  std::string username;
  bool admin = false;
};

// Everything a handler needs for one request.
struct Ctx {
  const Request& req;
  Services& svc;
  AuthUser user;
  db::Connection& db;  // read-only connection for this request
  std::string client;  // the `c` parameter

  std::optional<std::string> param(std::string_view name) const { return req.param(name); }
  std::vector<std::string> params(std::string_view name) const { return req.params_all(name); }
  std::string require(std::string_view name) const;
  // Integer parameter with a default, clamped to [min, max]. Non-numbers -> error 0.
  int64_t int_param(std::string_view name, int64_t def, int64_t min, int64_t max) const;
  std::optional<int64_t> opt_int(std::string_view name) const;
  bool bool_param(std::string_view name, bool def) const;

  void require_admin() const;
  // Runs a write on the WriteQueue and waits for it.
  template <class F>
  auto write(F&& fn) {
    return svc.writer.run(std::forward<F>(fn));
  }
};

}  // namespace eos::api
