#pragma once

#include <drogon/HttpRequest.h>

#include "api/http.h"

namespace eos::http {

// Drogon request -> transport-independent request (query and form parameters in order).
api::Request to_request(const drogon::HttpRequestPtr& req);

}  // namespace eos::http
