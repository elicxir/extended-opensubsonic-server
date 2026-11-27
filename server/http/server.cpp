#include "http/server.h"

#include "http/convert.h"

#include <drogon/drogon.h>

#include <stdexcept>
#include <thread>

#include "core/log.h"
#include "media/transcode.h"
#include "core/strings.h"

namespace eos::http {
namespace {

std::pair<std::string, uint16_t> split_listen(const std::string& listen) {
  auto colon = listen.rfind(':');
  if (colon == std::string::npos) throw std::invalid_argument("listen must be host:port");
  auto host = listen.substr(0, colon);
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  auto port = std::stoi(listen.substr(colon + 1));
  if (port <= 0 || port > 65535) throw std::invalid_argument("invalid port in listen");
  return {host, static_cast<uint16_t>(port)};
}

drogon::HttpResponsePtr to_drogon(const api::Response& r) {
  drogon::HttpResponsePtr resp;
  if (r.stream) {
    auto source = r.stream;
    resp = drogon::HttpResponse::newAsyncStreamResponse([source](drogon::ResponseStreamPtr out) {
      // Read the producer on its own thread; send() fails once the client is gone,
      // which makes the source stop (and kill ffmpeg).
      std::thread([source, out = std::move(out)]() mutable {
        source->run([&](const char* data, size_t n) { return out->send(std::string(data, n)); });
        out->close();
      }).detach();
    });
    resp->setContentTypeString(r.content_type);
  } else if (r.file) {
    bool partial = r.status == 206;
    resp = drogon::HttpResponse::newFileResponse(r.file->string(), static_cast<size_t>(r.offset),
                                                 static_cast<size_t>(r.length), partial, "",
                                                 drogon::CT_CUSTOM, r.content_type);
  } else {
    resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(r.body);
    resp->setContentTypeString(r.content_type);
  }
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(r.status));
  for (const auto& [k, v] : r.headers) {
    if (r.file && iequals_ascii(k, "Content-Range")) continue;  // Drogon sets it for file ranges
    resp->addHeader(k, v);
  }
  return resp;
}

}  // namespace

api::Request to_request(const drogon::HttpRequestPtr& req) {
  api::Request r;
  r.method = req->methodString();
  r.path = std::string(req->path());
  api::parse_query(req->query(), r.params);
  auto ctype = to_lower_ascii(req->getHeader("content-type"));
  if (req->method() == drogon::Post && ctype.starts_with("application/x-www-form-urlencoded"))
    api::parse_query(req->body(), r.params);  // formPost extension
  else
    r.body = std::string(req->body());
  for (const auto& [k, v] : req->getHeaders()) r.headers[to_lower_ascii(k)] = v;
  r.peer_ip = req->peerAddr().toIp();
  return r;
}

void run(const std::vector<Route>& routes, const std::string& listen, int threads, const std::string& scratch_dir) {
  auto [host, port] = split_listen(listen);
  if (threads <= 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  auto& app = drogon::app();
  app.setLogLevel(trantor::Logger::kWarn)
      .disableSigtermHandling()
      .enableServerHeader(false)
      .setThreadNum(static_cast<size_t>(threads))
      .setClientMaxBodySize(8 * 1024 * 1024)
      .setUploadPath(scratch_dir)
      .addListener(host, port);
  for (const auto& route : routes) {
    app.registerHandlerViaRegex(
        route.pattern,
        [handler = route.handler](const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
          auto request = to_request(req);
          auto response = handler(request);
          log::debug("{} {} -> {}", request.method, request.path, response.status);
          cb(to_drogon(response));
        },
        {drogon::Get, drogon::Post, drogon::Put, drogon::Patch, drogon::Delete, drogon::Head});
  }
  log::info("listening on {}:{} ({} threads)", host, port, threads);
  app.run();
}

void stop() { drogon::app().quit(); }

}  // namespace eos::http
