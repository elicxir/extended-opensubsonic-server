#pragma once

#include <functional>
#include <string>
#include <vector>

#include "api/http.h"

namespace eos::http {

using Handler = std::function<api::Response(const api::Request&)>;
struct Route {
  std::string pattern;  // regex on the path, e.g. "/rest/.*"
  Handler handler;
};

// Serves the routes through Drogon. Blocks until stop() is called from another thread.
// `listen` is "host:port" ("0.0.0.0:4540", "[::]:4540").
// `scratch_dir` is where Drogon may keep temporary upload files (it must be writable).
void run(const std::vector<Route>& routes, const std::string& listen, int threads, const std::string& scratch_dir);
void stop();

}  // namespace eos::http
