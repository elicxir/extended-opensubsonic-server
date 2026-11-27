#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "api/subsonic/context.h"

namespace eos::api {

// A handler adds its payload to the subsonic-response element `root` (ping adds nothing).
using Handler = std::function<void(Ctx&, Node& root)>;
// A raw handler returns the whole HTTP response (stream, download, getCoverArt).
using RawHandler = std::function<Response(Ctx&)>;

struct HandlerTable {
  std::unordered_map<std::string, Handler> handlers;
  std::unordered_map<std::string, RawHandler> raw;
  void add(const std::string& name, Handler h) { handlers[name] = std::move(h); }
  void add_raw(const std::string& name, RawHandler h) { raw[name] = std::move(h); }
};

void add_system_handlers(HandlerTable& t);
void add_browse_handlers(HandlerTable& t);
void add_list_handlers(HandlerTable& t);
void add_search_handlers(HandlerTable& t);
void add_annotation_handlers(HandlerTable& t);
void add_playlist_handlers(HandlerTable& t);
void add_queue_handlers(HandlerTable& t);
void add_media_handlers(HandlerTable& t);

// Dispatches /rest/<name>[.view] requests.
class Router {
 public:
  explicit Router(Services& svc);
  Response handle(const Request& req);
  bool knows(const std::string& endpoint) const;

 private:
  Services& svc_;
  HandlerTable table_;
};

// The subsonic-response envelope with status "ok" or "failed".
Node envelope(bool ok);
Response render(const Request& req, const Node& root, int http_status = 200);
Response error_response(const Request& req, int code, const std::string& message, int http_status = 200);

}  // namespace eos::api
