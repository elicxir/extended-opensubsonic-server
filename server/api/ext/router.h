#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "api/subsonic/context.h"

namespace eos::api::ext {

using json = nlohmann::ordered_json;

// An /ext error: HTTP status plus {"error": {"code", "message"}} (design: 独自拡張API).
struct ExtError : std::runtime_error {
  int status;
  std::string code;
  ExtError(int s, std::string c, const std::string& msg) : std::runtime_error(msg), status(s), code(std::move(c)) {}
};
inline ExtError bad_request(const std::string& m) { return {400, "bad_request", m}; }
inline ExtError not_found(const std::string& m) { return {404, "not_found", m}; }
inline ExtError forbidden(const std::string& m) { return {403, "forbidden", m}; }
inline ExtError conflict(const std::string& m) { return {409, "conflict", m}; }
inline ExtError tag_managed(const std::string& m) { return {409, "tag_managed", m}; }

struct Reply {
  int status = 200;
  json body;
  bool no_content = false;  // 204
};
inline Reply ok(json body) { return {200, std::move(body), false}; }
inline Reply created(json body) { return {201, std::move(body), false}; }
inline Reply no_content() { return {204, nullptr, true}; }
Reply page(json items, int64_t total);

struct ExtCtx {
  ExtCtx(const Request& r, Services& s, AuthUser u, db::Connection& d, std::vector<std::string> a)
      : req(r), svc(s), user(std::move(u)), db(d), args(std::move(a)) {}

  const Request& req;
  Services& svc;
  AuthUser user;
  db::Connection& db;
  std::vector<std::string> args;  // the {} parts of the route, in order

  // The JSON body (an object). Throws bad_request when missing or malformed.
  const nlohmann::json& body();
  int64_t limit() const;   // ?limit, default 100, max 500
  int64_t offset() const;  // ?offset
  // ?includeEstimated=false hides source = 1 rows. Returns the lowest source to show.
  int min_source() const;
  std::optional<std::string> query(std::string_view name) const { return req.param(name); }
  void require_admin() const;

  // Runs `fn` on the write connection inside a transaction. With `reindex`, the search
  // index is rebuilt in the same transaction (names changed).
  template <class F>
  auto write(F&& fn, bool reindex = false) {
    return svc.writer.run([&](db::Connection& w) {
      db::Transaction tx(w);
      if constexpr (std::is_void_v<std::invoke_result_t<F, db::Connection&>>) {
        fn(w);
        if (reindex) rebuild_index(w);
        tx.commit();
      } else {
        auto result = fn(w);
        if (reindex) rebuild_index(w);
        tx.commit();
        return result;
      }
    });
  }

 private:
  static void rebuild_index(db::Connection& w);
  std::optional<nlohmann::json> body_;
};

enum class Access { open, user, admin };
using ExtHandler = std::function<Reply(ExtCtx&)>;

class ExtRouter {
 public:
  explicit ExtRouter(Services& svc);
  Response handle(const Request& req);
  // `pattern` like "/ext/albums/{}/titles"; {} matches one path segment.
  void add(const std::string& method, const std::string& pattern, ExtHandler h, Access access = Access::user);

 private:
  struct Route {
    std::string method;
    std::vector<std::string> segments;
    ExtHandler handler;
    Access access;
  };
  Services& svc_;
  std::vector<Route> routes_;
};

void add_read_routes(ExtRouter& r);
void add_write_routes(ExtRouter& r);
void add_suggestion_routes(ExtRouter& r);
void add_account_routes(ExtRouter& r);
void add_transfer_routes(ExtRouter& r);

Response json_response(int status, const json& body);
Response error_response(const ExtError& e);

}  // namespace eos::api::ext
