#include "api/ext/router.h"

#include "api/subsonic/auth.h"
#include "core/log.h"
#include "core/strings.h"
#include "search/index.h"

namespace eos::api::ext {
namespace {

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> out;
  for (auto& s : split_any(path, {"/"})) out.push_back(url_decode(s, false));
  return out;
}

// Maps the Subsonic auth failure codes onto HTTP.
ExtError auth_error(const SubsonicError& e) {
  std::string msg = e.what();
  if (msg.starts_with("Too many")) return {429, "too_many_requests", msg};
  switch (e.code) {
    case err::missing_param:
    case err::wrong_credentials:
    case err::invalid_api_key:
      return {401, "unauthorized", msg};
    default:
      return {400, "bad_request", msg};
  }
}

}  // namespace

Reply page(json items, int64_t total) { return ok(json{{"items", std::move(items)}, {"total", total}}); }

const nlohmann::json& ExtCtx::body() {
  if (!body_) {
    if (trim(req.body).empty()) throw bad_request("A JSON body is required");
    try {
      body_ = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception& e) {
      throw bad_request(std::string("Malformed JSON: ") + e.what());
    }
    if (!body_->is_object()) throw bad_request("The JSON body must be an object");
  }
  return *body_;
}

int64_t ExtCtx::limit() const {
  auto v = req.param("limit");
  if (!v) return 100;
  try {
    return std::clamp<int64_t>(std::stoll(*v), 0, 500);
  } catch (...) {
    throw bad_request("limit must be an integer");
  }
}

int64_t ExtCtx::offset() const {
  auto v = req.param("offset");
  if (!v) return 0;
  try {
    return std::max<int64_t>(0, std::stoll(*v));
  } catch (...) {
    throw bad_request("offset must be an integer");
  }
}

int ExtCtx::min_source() const {
  auto v = req.param("includeEstimated");
  return v && (*v == "false" || *v == "0") ? 2 : 1;
}

void ExtCtx::require_admin() const {
  if (!user.admin) throw forbidden("Only administrators can change the library");
}

void ExtCtx::rebuild_index(db::Connection& w) { search::rebuild_index(w); }

Response json_response(int status, const json& body) {
  Response r;
  r.status = status;
  r.content_type = "application/json; charset=utf-8";
  if (!body.is_null()) r.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
  return r;
}

Response error_response(const ExtError& e) {
  return json_response(e.status, json{{"error", {{"code", e.code}, {"message", e.what()}}}});
}

ExtRouter::ExtRouter(Services& svc) : svc_(svc) {
  add_read_routes(*this);
  add_write_routes(*this);
  add_suggestion_routes(*this);
  add_account_routes(*this);
  add_transfer_routes(*this);
  add_graph_routes(*this);
}

void ExtRouter::add(const std::string& method, const std::string& pattern, ExtHandler h, Access access) {
  routes_.push_back({method, split_path(pattern), std::move(h), access});
}

Response ExtRouter::handle(const Request& req) {
  const auto path = split_path(req.path);
  const std::string method = req.method == "HEAD" ? "GET" : req.method;
  const Route* route = nullptr;
  std::vector<std::string> args;
  bool path_known = false;
  for (const auto& r : routes_) {
    if (r.segments.size() != path.size()) continue;
    std::vector<std::string> a;
    bool match = true;
    for (size_t i = 0; i < path.size() && match; ++i) {
      if (r.segments[i] == "{}") a.push_back(path[i]);
      else match = r.segments[i] == path[i];
    }
    if (!match) continue;
    path_known = true;
    if (r.method != method) continue;
    route = &r;
    args = std::move(a);
    break;
  }
  if (!route) {
    return path_known ? error_response({405, "method_not_allowed", "Method not allowed"})
                      : error_response(not_found("No such endpoint"));
  }

  try {
    auto lease = svc_.reads.acquire();
    AuthUser user;
    if (route->access != Access::open) {
      try {
        user = authenticate(req, svc_, *lease);
      } catch (const SubsonicError& e) {
        throw auth_error(e);
      }
    }
    ExtCtx ctx{req, svc_, user, *lease, std::move(args)};
    if (route->access == Access::admin) ctx.require_admin();
    auto reply = route->handler(ctx);
    if (reply.no_content) return json_response(204, nullptr);
    return json_response(reply.status, reply.body);
  } catch (const ExtError& e) {
    return error_response(e);
  } catch (const nlohmann::json::exception& e) {
    return error_response(bad_request(std::string("Invalid JSON value: ") + e.what()));
  } catch (const db::DbError& e) {
    if (e.code == SQLITE_CONSTRAINT) return error_response(conflict(e.what()));
    log::error("{} {} failed: {}", req.method, req.path, e.what());
    return error_response({500, "internal", "Internal server error"});
  } catch (const std::exception& e) {
    log::error("{} {} failed: {}", req.method, req.path, e.what());
    return error_response({500, "internal", "Internal server error"});
  }
}

}  // namespace eos::api::ext
