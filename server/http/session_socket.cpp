#include "http/session_socket.h"

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>

#include "core/log.h"
#include "http/convert.h"

namespace eos::http {
namespace {

// Hands hub messages to one WebSocket. Holding the connection weakly lets a closed
// socket go away even while the hub still has the session.
class SocketSink : public sync::Sink {
 public:
  explicit SocketSink(const drogon::WebSocketConnectionPtr& conn) : conn_(conn) {}
  void send(const std::string& text) override {
    if (auto c = conn_.lock(); c && c->connected()) c->send(text);
  }

 private:
  std::weak_ptr<drogon::WebSocketConnection> conn_;
};

class SessionSocket : public drogon::WebSocketController<SessionSocket, false> {
 public:
  SessionSocket(sync::SessionHub& hub, SocketAuth auth) : hub_(hub), auth_(std::move(auth)) {}

  void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override {
    auto user = auth_(to_request(req));
    if (!user) {
      conn->send(R"({"type":"error","message":"Unauthorized"})");
      conn->shutdown(drogon::CloseCode::kViolation, "unauthorized");
      return;
    }
    auto session = hub_.connect(*user, std::make_shared<SocketSink>(conn));
    conn->setContext(session);
    log::debug("sync: user {} connected", *user);
  }

  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                        const drogon::WebSocketMessageType& type) override {
    if (type != drogon::WebSocketMessageType::Text) return;  // pings are answered by Drogon
    if (auto session = conn->getContext<sync::Session>()) hub_.receive(session, message);
  }

  void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
    if (auto session = conn->getContext<sync::Session>()) {
      hub_.disconnect(session);
      conn->clearContext();
    }
  }

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/ext/session");
  WS_PATH_LIST_END

 private:
  sync::SessionHub& hub_;
  SocketAuth auth_;
};

}  // namespace

void register_session_socket(sync::SessionHub& hub, SocketAuth auth) {
  static std::shared_ptr<SessionSocket> socket;  // lives as long as the server
  socket = std::make_shared<SessionSocket>(hub, std::move(auth));
  drogon::app().registerController(socket);
}

}  // namespace eos::http
