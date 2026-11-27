#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "api/http.h"
#include "sync/hub.h"

namespace eos::http {

// Returns the user id for an authenticated upgrade request, or nullopt.
using SocketAuth = std::function<std::optional<int64_t>(const api::Request&)>;

// Registers the WebSocket endpoint GET /ext/session with Drogon. Call before http::run.
// Authentication is the same as for /ext (X-Api-Key header or apiKey query).
void register_session_socket(sync::SessionHub& hub, SocketAuth auth);

}  // namespace eos::http
