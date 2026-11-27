#include "api/subsonic/router.h"

#include "api/subsonic/auth.h"
#include "core/log.h"
#include "core/strings.h"

namespace eos::api {
namespace {

// Endpoints that answer without authentication.
bool is_public(const std::string& name) { return name == "getOpenSubsonicExtensions"; }

bool valid_callback(const std::string& cb) {
  if (cb.empty() || cb.size() > 128) return false;
  for (unsigned char c : cb)
    if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) return false;
  return true;
}

}  // namespace

Node envelope(bool ok) {
  Node n("subsonic-response");
  n.set("status", ok ? "ok" : "failed")
      .set("version", kApiVersion)
      .set("type", kServerType)
      .set("serverVersion", EOS_VERSION)
      .set("openSubsonic", true);
  return n;
}

Response render(const Request& req, const Node& root, int http_status) {
  Response r;
  r.status = http_status;
  auto f = to_lower_ascii(req.param("f").value_or("xml"));
  if (f == "json") {
    r.content_type = "application/json; charset=utf-8";
    r.body = to_json(root);
  } else if (f == "jsonp") {
    auto cb = req.param("callback").value_or("callback");
    if (!valid_callback(cb)) cb = "callback";
    r.content_type = "application/javascript; charset=utf-8";
    r.body = cb + "(" + to_json(root) + ");";
  } else {
    r.content_type = "application/xml; charset=utf-8";
    r.body = to_xml(root);
  }
  return r;
}

Response error_response(const Request& req, int code, const std::string& message, int http_status) {
  auto root = envelope(false);
  root.child(Node("error").set("code", code).set("message", message));
  return render(req, root, http_status);
}

Router::Router(Services& svc) : svc_(svc) {
  add_system_handlers(table_);
  add_browse_handlers(table_);
  add_list_handlers(table_);
  add_search_handlers(table_);
  add_annotation_handlers(table_);
  add_playlist_handlers(table_);
  add_queue_handlers(table_);
  add_media_handlers(table_);
}

bool Router::knows(const std::string& endpoint) const {
  return table_.handlers.count(endpoint) || table_.raw.count(endpoint);
}

Response Router::handle(const Request& req) {
  std::string name = req.path;
  if (!name.starts_with("/rest/")) return error_response(req, err::not_found, "Not found", 404);
  name = name.substr(6);
  if (name.ends_with(".view")) name.resize(name.size() - 5);

  auto h = table_.handlers.find(name);
  auto raw = table_.raw.find(name);
  if (h == table_.handlers.end() && raw == table_.raw.end())
    return error_response(req, err::not_found, "Unknown endpoint: " + name, 404);

  try {
    auto lease = svc_.reads.acquire();
    AuthUser user;
    if (!is_public(name)) user = authenticate(req, svc_, *lease);
    Ctx ctx{req, svc_, user, *lease, req.param("c").value_or("")};
    if (raw != table_.raw.end()) return raw->second(ctx);
    auto root = envelope(true);
    h->second(ctx, root);
    return render(req, root);
  } catch (const SubsonicError& e) {
    return error_response(req, e.code, e.what());
  } catch (const std::exception& e) {
    log::error("{} failed: {}", name, e.what());
    return error_response(req, err::generic, "Internal server error");
  }
}

}  // namespace eos::api
