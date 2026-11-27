// /ext/auth/login and per-device API keys (design: 認証とセキュリティ).
#include "api/ext/model.h"
#include "api/subsonic/auth.h"
#include "auth/users.h"

namespace eos::api::ext {

void add_account_routes(ExtRouter& r) {
  // The one write that needs no credentials: trade a password for a device key.
  r.add(
      "POST", "/ext/auth/login",
      [](ExtCtx& c) {
        const auto& b = c.body();
        auto username = b.value("username", "");
        auto password = b.value("password", "");
        auto device = b.value("deviceName", "");
        if (username.empty() || password.empty() || device.empty())
          throw bad_request("username, password and deviceName are required");

        const auto ip = client_ip(c.req.peer_ip, c.req.header("x-forwarded-for"), c.svc.trusted_proxies);
        const bool exempt = c.svc.rate_limit_exempt.contains(ip);
        const std::string ip_key = "ip:" + ip, user_key = "user:" + username;
        if (!exempt && (c.svc.limiter.blocked(ip_key) || c.svc.limiter.blocked(user_key)))
          throw ExtError(429, "too_many_requests", "Too many failed attempts; try again later");
        auto user = eos::auth::find_user(c.db, username);
        if (!user || !eos::auth::verify_password(c.db, user->id, password)) {
          if (!exempt) {
            c.svc.limiter.failure(ip_key);
            c.svc.limiter.failure(user_key);
          }
          throw ExtError(401, "unauthorized", "Wrong username or password");
        }
        c.svc.limiter.success(ip_key);
        c.svc.limiter.success(user_key);
        auto key = c.write([&](db::Connection& w) { return eos::auth::create_api_key(w, user->id, device); });
        return created({{"id", key.id}, {"key", key.key}, {"username", user->username}, {"admin", user->is_admin}});
      },
      Access::open);

  r.add("GET", "/ext/apikeys", [](ExtCtx& c) {
    json items = json::array();
    for (const auto& k : eos::auth::list_api_keys(c.db, c.user.id))
      items.push_back({{"id", k.id},
                       {"deviceName", k.device_name},
                       {"createdAt", iso_time(k.created_at)},
                       {"lastUsedAt", k.last_used_at ? json(iso_time(*k.last_used_at)) : json(nullptr)},
                       {"revoked", k.revoked_at.has_value()}});
    auto n = static_cast<int64_t>(items.size());
    return page(std::move(items), n);
  });

  r.add("POST", "/ext/apikeys", [](ExtCtx& c) {
    auto device = c.body().value("deviceName", "");
    if (device.empty()) throw bad_request("deviceName is required");
    auto key = c.write([&](db::Connection& w) { return eos::auth::create_api_key(w, c.user.id, device); });
    return created({{"id", key.id}, {"key", key.key}, {"deviceName", device}});
  });

  r.add("DELETE", "/ext/apikeys/{}", [](ExtCtx& c) {
    auto owner = c.db.scalar_int("SELECT user_id FROM api_key WHERE id = ?", c.args[0]);
    if (!owner || (*owner != c.user.id && !c.user.admin)) throw not_found("API key not found");
    c.write([&](db::Connection& w) { eos::auth::revoke_api_key(w, c.args[0]); });
    return no_content();
  });
}

}  // namespace eos::api::ext
