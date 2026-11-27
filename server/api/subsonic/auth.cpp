#include "api/subsonic/auth.h"

#include <openssl/evp.h>

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "auth/users.h"
#include "core/log.h"
#include "core/strings.h"

namespace eos::api {
namespace {

// Argon2id costs ~64 MiB and tens of milliseconds, too much for every request of a
// client that uses p= auth. Remember successful checks for a few minutes.
class PasswordCache {
 public:
  bool hit(int64_t user_id, std::string_view password) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(user_id);
    return it != entries_.end() && it->second.expires > Clock::now() &&
           crypto::equal(it->second.digest, digest(password));
  }
  void put(int64_t user_id, std::string_view password) {
    std::lock_guard lock(mutex_);
    entries_[user_id] = {digest(password), Clock::now() + std::chrono::minutes(10)};
  }

 private:
  using Clock = std::chrono::steady_clock;
  static std::string digest(std::string_view pw) {
    auto d = crypto::sha256(pw);
    return std::string(d.begin(), d.end());
  }
  struct Entry {
    std::string digest;
    Clock::time_point expires;
  };
  std::mutex mutex_;
  std::unordered_map<int64_t, Entry> entries_;
};

PasswordCache g_password_cache;

std::optional<std::string> hex_decode(std::string_view hex) {
  if (hex.size() % 2) return std::nullopt;
  std::string out;
  for (size_t i = 0; i < hex.size(); i += 2) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = nib(hex[i]), lo = nib(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<char>(hi * 16 + lo));
  }
  return out;
}

std::optional<AuthUser> load_user(db::Connection& c, std::string_view username) {
  auto st = c.prepare("SELECT id, username, is_admin FROM user WHERE username = ?");
  st.bind(1, username);
  if (!st.step()) return std::nullopt;
  return AuthUser{st.int64(0), st.text(1), st.int64(2) != 0};
}

}  // namespace

std::string md5_hex(std::string_view data) {
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(data.data(), data.size(), out, &len, EVP_md5(), nullptr);
  return crypto::hex(out, len);
}

AuthUser authenticate(const Request& req, Services& svc, db::Connection& conn) {
  const auto ip = client_ip(req.peer_ip, req.header("x-forwarded-for"), svc.trusted_proxies);
  const bool exempt = svc.rate_limit_exempt.contains(ip);

  auto api_key = req.param("apiKey");
  if (!api_key) api_key = req.header("x-api-key");
  auto u = req.param("u");
  auto t = req.param("t");
  auto s = req.param("s");
  auto p = req.param("p");
  const bool has_token = t || s;

  if (api_key && (u || has_token || p))
    throw SubsonicError(err::conflicting_auth, "Multiple conflicting authentication mechanisms provided");
  if (has_token && p)
    throw SubsonicError(err::conflicting_auth, "Multiple conflicting authentication mechanisms provided");
  if (!api_key && !u) throw SubsonicError(err::missing_param, "Required parameter is missing: u");
  if (!api_key && !has_token && !p)
    throw SubsonicError(err::missing_param, "Required parameter is missing: apiKey, t/s or p");

  const std::string ip_key = "ip:" + ip;
  const std::string user_key = u ? "user:" + *u : std::string();
  auto fail = [&](int code, const std::string& msg) -> SubsonicError {
    if (!exempt) {
      svc.limiter.failure(ip_key);
      if (!user_key.empty()) svc.limiter.failure(user_key);
    }
    log::info("auth failed from {} ({})", ip, msg);
    return SubsonicError(code, msg);
  };
  if (!exempt && (svc.limiter.blocked(ip_key) || (!user_key.empty() && svc.limiter.blocked(user_key))))
    throw SubsonicError(api_key ? err::invalid_api_key : err::wrong_credentials,
                        "Too many failed attempts; try again later");

  AuthUser user;
  if (api_key) {
    auto hash = crypto::sha256(*api_key);
    auto st = conn.prepare(
        "SELECT k.id, u.id, u.username, u.is_admin FROM api_key k JOIN user u ON u.id = k.user_id "
        "WHERE k.key_hash = ? AND k.revoked_at IS NULL");
    st.bind_blob(1, hash.data(), hash.size());
    if (!st.step()) throw fail(err::invalid_api_key, "Invalid API key");
    svc.touch_api_key(st.text(0));
    user = {st.int64(1), st.text(2), st.int64(3) != 0};
  } else if (has_token) {
    if (!svc.cfg.allow_token)
      throw SubsonicError(err::token_not_supported, "Token authentication is disabled on this server");
    if (!t || !s) throw SubsonicError(err::missing_param, "Required parameter is missing: t or s");
    auto found = load_user(conn, *u);
    if (!found) throw fail(err::wrong_credentials, "Wrong username or password");
    auto password = eos::auth::decrypt_password(conn, found->id, svc.secret_key);
    if (!password || !crypto::equal(md5_hex(*password + *s), to_lower_ascii(*t)))
      throw fail(err::wrong_credentials, "Wrong username or password");
    user = *found;
  } else {
    if (!svc.cfg.allow_password)
      throw SubsonicError(err::auth_not_supported, "Password authentication is disabled on this server");
    std::string password = *p;
    if (starts_with_ci(password, "enc:")) {
      auto decoded = hex_decode(std::string_view(password).substr(4));
      if (!decoded) throw fail(err::wrong_credentials, "Wrong username or password");
      password = *decoded;
    }
    auto found = load_user(conn, *u);
    if (!found) throw fail(err::wrong_credentials, "Wrong username or password");
    if (!g_password_cache.hit(found->id, password)) {
      if (!eos::auth::verify_password(conn, found->id, password))
        throw fail(err::wrong_credentials, "Wrong username or password");
      g_password_cache.put(found->id, password);
    }
    user = *found;
  }

  svc.limiter.success(ip_key);
  if (!user_key.empty()) svc.limiter.success(user_key);
  return user;
}

}  // namespace eos::api
