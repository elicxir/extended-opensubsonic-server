#pragma once

#include "api/subsonic/context.h"

namespace eos::api {

// Authenticates a /rest or /ext request (design: 認証とセキュリティ). Throws
// SubsonicError with the OpenSubsonic error code on failure.
//   apiKey (query) or X-Api-Key (header) -> API key
//   u + t + s                            -> token auth (md5(password + salt))
//   u + p                                -> password auth ("enc:" hex allowed)
AuthUser authenticate(const Request& req, Services& svc, db::Connection& conn);

std::string md5_hex(std::string_view data);

}  // namespace eos::api
