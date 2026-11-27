#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/http.h"
#include "api/net.h"
#include "api/node.h"
#include "api/rate_limit.h"

using namespace eos::api;

TEST(Node, JsonShapes) {
  Node root("subsonic-response");
  root.set("status", "ok").set("openSubsonic", true);
  Node album("album");
  album.set("id", "a1").set("songCount", 3).set("name", "A & B");
  album.list("song", {Node("song").set("id", "s1"), Node("song").set("id", "s2")});
  album.list("genres", {}, true);
  album.list("artists", {});  // dropped when empty
  root.child(std::move(album));
  root.child(Node("genre").set("songCount", 2).text("Rock"));
  root.child(Node("ext").scalars("versions", {Value(int64_t{1}), Value(int64_t{2})}));

  auto j = nlohmann::json::parse(to_json(root))["subsonic-response"];
  EXPECT_EQ(j["status"], "ok");
  EXPECT_EQ(j["openSubsonic"], true);
  EXPECT_EQ(j["album"]["songCount"], 3);
  EXPECT_EQ(j["album"]["song"].size(), 2u);
  EXPECT_TRUE(j["album"]["genres"].is_array());
  EXPECT_FALSE(j["album"].contains("artists"));
  EXPECT_EQ(j["genre"]["value"], "Rock");
  EXPECT_EQ(j["ext"]["versions"], nlohmann::json::array({1, 2}));
}

TEST(Node, XmlShapesAndEscaping) {
  Node root("subsonic-response");
  root.set("status", "ok");
  root.child(Node("album").set("name", "A & \"B\" <C>").set("starred", false));
  root.child(Node("genre").text("R&B"));
  root.child(Node("ext").scalars("versions", {Value(int64_t{1})}));
  auto xml = to_xml(root);
  EXPECT_NE(xml.find(R"(<subsonic-response xmlns="http://subsonic.org/restapi" status="ok">)"), std::string::npos);
  EXPECT_NE(xml.find(R"(<album name="A &amp; &quot;B&quot; &lt;C&gt;" starred="false"/>)"), std::string::npos);
  EXPECT_NE(xml.find("<genre>R&amp;B</genre>"), std::string::npos);
  EXPECT_NE(xml.find("<ext><versions>1</versions></ext>"), std::string::npos);
}

TEST(Http, QueryParsingKeepsDuplicatesAndDecodes) {
  std::vector<std::pair<std::string, std::string>> p;
  parse_query("id=a&id=b&query=%E3%81%82+x&flag", p);
  Request r;
  r.params = p;
  EXPECT_EQ(r.params_all("id"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(r.param("query"), "あ x");
  EXPECT_EQ(r.param("flag"), "");
  EXPECT_EQ(r.param("none"), std::nullopt);
}

TEST(Http, ByteRanges) {
  ByteRange r;
  EXPECT_EQ(parse_range("bytes=0-99", 1000, r), RangeResult::ok);
  EXPECT_EQ(r.start, 0);
  EXPECT_EQ(r.length, 100);
  EXPECT_EQ(parse_range("bytes=900-", 1000, r), RangeResult::ok);
  EXPECT_EQ(r.length, 100);
  EXPECT_EQ(parse_range("bytes=-10", 1000, r), RangeResult::ok);
  EXPECT_EQ(r.start, 990);
  EXPECT_EQ(parse_range("bytes=990-5000", 1000, r), RangeResult::ok);
  EXPECT_EQ(r.length, 10);
  EXPECT_EQ(parse_range("bytes=1000-", 1000, r), RangeResult::unsatisfiable);
  EXPECT_EQ(parse_range("bytes=0-1,5-6", 1000, r), RangeResult::none);
  EXPECT_EQ(parse_range("items=0-1", 1000, r), RangeResult::none);
}

TEST(Net, CidrAndClientIp) {
  IpSet lan({"127.0.0.0/8", "::1", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "fc00::/7", "fe80::/10"});
  EXPECT_TRUE(lan.contains("127.0.0.1"));
  EXPECT_TRUE(lan.contains("192.168.1.20"));
  EXPECT_TRUE(lan.contains("172.31.255.1"));
  EXPECT_FALSE(lan.contains("172.32.0.1"));
  EXPECT_TRUE(lan.contains("::1"));
  EXPECT_TRUE(lan.contains("fd12::5"));
  EXPECT_TRUE(lan.contains("::ffff:10.1.2.3"));
  EXPECT_FALSE(lan.contains("203.0.113.5"));
  EXPECT_FALSE(lan.contains("garbage"));

  IpSet proxies({"127.0.0.1"});
  EXPECT_EQ(client_ip("127.0.0.1", "203.0.113.9", proxies), "203.0.113.9");
  EXPECT_EQ(client_ip("127.0.0.1", "198.51.100.1, 203.0.113.9", proxies), "203.0.113.9");
  // The header is ignored when the peer is not a trusted proxy.
  EXPECT_EQ(client_ip("198.51.100.7", "10.0.0.1", proxies), "198.51.100.7");
}

TEST(RateLimiter, BlocksAfterTooManyFailures) {
  RateLimiter rl(3, std::chrono::seconds(60), std::chrono::seconds(120));
  auto t0 = RateLimiter::Clock::now();
  rl.failure("ip:x", t0);
  rl.failure("ip:x", t0);
  EXPECT_FALSE(rl.blocked("ip:x", t0));
  rl.failure("ip:x", t0);
  EXPECT_TRUE(rl.blocked("ip:x", t0 + std::chrono::seconds(1)));
  EXPECT_FALSE(rl.blocked("ip:x", t0 + std::chrono::seconds(121)));
  // Failures outside the window do not add up.
  rl.failure("ip:y", t0);
  rl.failure("ip:y", t0 + std::chrono::seconds(61));
  rl.failure("ip:y", t0 + std::chrono::seconds(122));
  EXPECT_FALSE(rl.blocked("ip:y", t0 + std::chrono::seconds(122)));
}
