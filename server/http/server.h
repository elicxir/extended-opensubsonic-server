#pragma once

#include <string>

#include "api/subsonic/router.h"

namespace eos::http {

// Serves /rest/* through Drogon. Blocks until stop() is called from another thread.
// `listen` is "host:port" ("0.0.0.0:4540", "[::]:4540").
// `scratch_dir` is where Drogon may keep temporary upload files (it must be writable).
void run(api::Router& router, const std::string& listen, int threads, const std::string& scratch_dir);
void stop();

}  // namespace eos::http
