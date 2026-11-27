#include <gtest/gtest.h>

#include "eos/client/api.h"
#include "http.h"

using namespace eos::client;

TEST(Http, UrlEncode) {
  EXPECT_EQ(url_encode("abc-_.~XYZ019"), "abc-_.~XYZ019");
  EXPECT_EQ(url_encode("a b&c=d/e?"), "a%20b%26c%3Dd%2Fe%3F");
  EXPECT_EQ(url_encode("バトル"), "%E3%83%90%E3%83%88%E3%83%AB");
  EXPECT_EQ(encode_query({{"id", "1"}, {"id", "2"}, {"q", "x y"}}), "id=1&id=2&q=x%20y");
}

TEST(Http, UrlJoinsBaseAndDropsTrailingSlash) {
  Http http({.base_url = "http://nas:4540/eos//", .api_key = "k"});
  EXPECT_EQ(http.url("/rest/ping"), "http://nas:4540/eos/rest/ping");
  EXPECT_EQ(http.url("/ext/search", {{"q", "a b"}}), "http://nas:4540/eos/ext/search?q=a%20b");
}

TEST(Api, MediaUrlsCarryTheKey) {
  Api api({.base_url = "http://nas:4540", .api_key = "secret", .client_name = "kleulir"});
  EXPECT_EQ(api.stream_url("s1"), "http://nas:4540/rest/stream?id=s1&apiKey=secret&v=1.16.1&c=kleulir");
  EXPECT_EQ(api.stream_url("s1", {.format = "opus", .max_bit_rate = 128, .time_offset = 30}),
            "http://nas:4540/rest/stream?id=s1&format=opus&maxBitRate=128&timeOffset=30&apiKey=secret&v=1.16.1&c=kleulir");
  EXPECT_EQ(api.cover_art_url("al-x_1", 300),
            "http://nas:4540/rest/getCoverArt?id=al-x_1&size=300&apiKey=secret&v=1.16.1&c=kleulir");
  EXPECT_EQ(api.download_url("s1"), "http://nas:4540/rest/download?id=s1&apiKey=secret&v=1.16.1&c=kleulir");
}

TEST(Api, UnreachableServerIsANetworkError) {
  // Port 1 on localhost: nothing listens there.
  Api api({.base_url = "http://127.0.0.1:1", .api_key = "k", .connect_timeout_ms = 2000, .timeout_ms = 2000});
  try {
    api.ping();
    FAIL() << "expected an Error";
  } catch (const Error& e) {
    EXPECT_EQ(e.kind(), Error::Kind::network);
    EXPECT_TRUE(e.offline());
    EXPECT_EQ(e.http_status(), 0);
  }
}

TEST(Api, NoAddressIsANetworkError) {
  Api api(Options{});
  EXPECT_THROW(api.ping(), Error);
}

TEST(Api, TokenIsMd5OfPasswordAndSalt) {
  // The example in the Subsonic API documentation.
  EXPECT_EQ(Api::make_token("sesame", "c19b2d"), "26719a1196d2a940705a59634eb18eab");
  EXPECT_EQ(Api::make_token("", ""), "d41d8cd98f00b204e9800998ecf8427e");
  // Longer than one MD5 block.
  EXPECT_EQ(Api::make_token("The quick brown fox jumps over the lazy dog", ""), "9e107d9d372bb6826bd81d3542a419d6");
  EXPECT_EQ(Api::make_token(std::string(64, 'a'), ""), "014842d480b571495a4a0363793f7367");
}

TEST(Api, MediaUrlsCarryTheTokenWithoutAKey) {
  Api api({.base_url = "http://nd:4533", .client_name = "kleulir", .username = "alice", .token = "abc", .salt = "s1"});
  EXPECT_EQ(api.stream_url("s1"), "http://nd:4533/rest/stream?id=s1&u=alice&t=abc&s=s1&v=1.16.1&c=kleulir");
}
