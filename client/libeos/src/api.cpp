#include "impl.h"
#include "parse.h"

namespace eos::client {
namespace {

constexpr const char* kSubsonicVersion = "1.16.1";

std::string short_body(const std::string& body) {
  return body.size() > 200 ? body.substr(0, 200) + "..." : body;
}

}  // namespace

Api::Api(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Api::~Api() = default;
Api::Api(Api&&) noexcept = default;
Api& Api::operator=(Api&&) noexcept = default;

const Options& Api::options() const { return impl_->http.options(); }

json Api::Impl::rest(const std::string& endpoint, Query params) {
  params.emplace_back("f", "json");
  params.emplace_back("v", kSubsonicVersion);
  params.emplace_back("c", http.options().client_name);
  auto res = http.send("POST", "/rest/" + endpoint, {}, encode_query(params), "application/x-www-form-urlencoded");

  auto doc = json::parse(res.body, nullptr, false);
  auto it = doc.is_object() ? doc.find("subsonic-response") : doc.end();
  if (doc.is_discarded() || !doc.is_object() || it == doc.end() || !it->is_object()) {
    if (res.status >= 400)
      throw Error(Error::Kind::http, "HTTP " + std::to_string(res.status) + " from /rest/" + endpoint, res.status);
    throw Error(Error::Kind::parse, "Not a Subsonic response from /rest/" + endpoint + ": " + short_body(res.body),
                res.status);
  }
  json root = std::move(*it);
  if (parse::get<std::string>(root, "status") != "ok") {
    const json& e = parse::object(root, "error");
    throw Error(Error::Kind::subsonic, parse::get<std::string>(e, "message", "Request failed"), res.status,
                parse::get<int>(e, "code", 0));
  }
  return root;
}

std::string Api::Impl::ext_raw(const std::string& method, const std::string& path, const Query& query,
                               const std::string& body) {
  auto res = http.send(method, path, query, body, body.empty() ? "" : "application/json");
  if (res.status >= 200 && res.status < 300) return std::move(res.body);

  auto doc = json::parse(res.body, nullptr, false);
  if (doc.is_object() && doc.contains("error") && doc["error"].is_object()) {
    const json& e = doc["error"];
    throw Error(Error::Kind::ext, parse::get<std::string>(e, "message", "Request failed"), res.status, -1,
                parse::get<std::string>(e, "code"));
  }
  throw Error(Error::Kind::http, "HTTP " + std::to_string(res.status) + " from " + path, res.status);
}

json Api::Impl::ext(const std::string& method, const std::string& path, Query query, const json* body) {
  const std::string text = ext_raw(method, path, query, body ? body->dump() : std::string());
  if (text.empty()) return nullptr;
  auto doc = json::parse(text, nullptr, false);
  if (doc.is_discarded()) throw Error(Error::Kind::parse, "Not JSON from " + path + ": " + short_body(text));
  return doc;
}

json Api::Impl::ext_get(const std::string& path, Query query) {
  if (!http.options().include_estimated) query.emplace_back("includeEstimated", "false");
  return ext("GET", path, std::move(query));
}

Query Api::Impl::media_query(Query params) const {
  params.emplace_back("apiKey", http.options().api_key);
  params.emplace_back("v", kSubsonicVersion);
  params.emplace_back("c", http.options().client_name);
  return params;
}

std::string Api::ext_request(const std::string& method, const std::string& path, const Query& query,
                             const std::string& json_body) {
  return impl_->ext_raw(method, path, query, json_body);
}

}  // namespace eos::client
