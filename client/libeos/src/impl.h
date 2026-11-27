#pragma once

#include <nlohmann/json.hpp>

#include "eos/client/api.h"
#include "http.h"

namespace eos::client {

using nlohmann::json;

struct Api::Impl {
  explicit Impl(Options o) : http(std::move(o)) {}

  // Calls /rest/<endpoint> (form POST, so long id lists and search words stay out of
  // the URL) and returns the "subsonic-response" object. Throws on status "failed".
  json rest(const std::string& endpoint, Query params = {});

  // Calls an /ext path and returns the parsed body (null for 204). `body` is sent as
  // JSON when not null. Throws on an error status.
  json ext(const std::string& method, const std::string& path, Query query = {}, const json* body = nullptr);
  // GET with the Options::include_estimated setting applied.
  json ext_get(const std::string& path, Query query = {});

  // The raw exchange behind ext(): returns the body text, throws on an error status.
  std::string ext_raw(const std::string& method, const std::string& path, const Query& query,
                      const std::string& body);

  // Query for the media URLs (stream, download, getCoverArt): key and client name.
  Query media_query(Query params) const;
  // Adds u, t and s when the session is by Subsonic token instead of an API key.
  void add_token(Query& params) const;

  Http http;
};

// Runs a parser, turning JSON type errors into Error(parse).
template <class F>
auto parsed(const char* what, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const json::exception& e) {
    throw Error(Error::Kind::parse, std::string("Unexpected response from ") + what + ": " + e.what());
  }
}

}  // namespace eos::client
