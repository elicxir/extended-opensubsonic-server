// End-to-end tests of /rest through the Router (no network): a scanned library of
// generated FLAC files, real users and API keys.
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

#include "api/subsonic/auth.h"
#include "api/subsonic/router.h"
#include "auth/users.h"
#include "core/config.h"
#include "db/migrate.h"
#include "fixtures.h"

using namespace eos;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

class ApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!test::have_flac_encoder()) GTEST_SKIP() << "flac encoder not installed";
    music = dir.path() / "music";
    cfg.data_dir = dir.path() / "data";
    cfg.libraries = {{"Music", music}};
    cfg.tags = default_custom_tags();
    fs::create_directories(cfg.data_dir);

    test::make_flac(music / "A/01.flac", 440,
                    {{"TITLE", {"Opening"}}, {"ALBUM", {"Album A"}}, {"ARTIST", {"Alice"}},
                     {"ALBUMARTIST", {"Alice"}}, {"COMPOSER", {"Carol"}}, {"TRACKNUMBER", {"1"}},
                     {"DISCNUMBER", {"1"}}, {"DISCSUBTITLE", {"Disc One"}}, {"GENRE", {"Soundtrack"}},
                     {"PERFORMER", {"Dave (piano)"}}, {"DATE", {"2020-04-01"}}});
    test::make_flac(music / "A/02.flac", 550,
                    {{"TITLE", {"Battle"}}, {"ALBUM", {"Album A"}}, {"ARTIST", {"Alice; Bob"}},
                     {"ALBUMARTIST", {"Alice"}}, {"TRACKNUMBER", {"2"}}, {"DISCNUMBER", {"1"}},
                     {"GENRE", {"Soundtrack"}}, {"DATE", {"2020-04-01"}}});
    test::make_flac(music / "B/01.flac", 660,
                    {{"TITLE", {"バトルのテーマ"}}, {"ALBUM", {"Album B"}}, {"ARTIST", {"Erin"}},
                     {"ALBUMARTIST", {"The Erin Band"}}, {"TRACKNUMBER", {"1"}}, {"GENRE", {"Rock"}},
                     {"DATE", {"1999"}}});
    test::add_flac_picture(music / "A/01.flac", test::make_png(64, 32), "image/png");

    {
      db::Connection c(cfg.db_path());
      db::migrate(c);
      key = crypto::random_bytes(32);
      alice_id = auth::create_user(c, "alice", "pw-alice", true, key).id;
      bob_id = auth::create_user(c, "bob", "pw-bob", false, key).id;
      alice_key = auth::create_api_key(c, alice_id, "test").key;
      bob_key = auth::create_api_key(c, bob_id, "test").key;
    }
    writer = std::make_unique<db::WriteQueue>(cfg.db_path());
    scanner = std::make_unique<scanner::Scanner>(cfg, *writer, cfg.db_path());
    scanner->scan(false);
    reads = std::make_unique<db::ReadPool>(cfg.db_path(), 2);
    services = std::make_unique<api::Services>(cfg, *reads, *writer, *scanner, key);
    router = std::make_unique<api::Router>(*services);
    conn = std::make_unique<db::Connection>(cfg.db_path());
  }

  api::Response raw(const std::string& endpoint,
                    std::vector<std::pair<std::string, std::string>> params = {},
                    std::map<std::string, std::string> headers = {}, std::string peer = "192.168.1.10") {
    api::Request r;
    r.path = "/rest/" + endpoint;
    r.params = std::move(params);
    r.headers = std::move(headers);
    r.peer_ip = std::move(peer);
    return router->handle(r);
  }

  // Calls with alice's API key (or `as_key`) and returns subsonic-response as JSON.
  json call(const std::string& endpoint, std::vector<std::pair<std::string, std::string>> params = {},
            const std::string* as_key = nullptr) {
    params.emplace_back("apiKey", as_key ? *as_key : alice_key);
    params.emplace_back("f", "json");
    params.emplace_back("c", "tests");
    auto r = raw(endpoint, std::move(params));
    EXPECT_EQ(r.content_type, "application/json; charset=utf-8") << endpoint;
    return json::parse(r.body)["subsonic-response"];
  }
  static int error_code(const json& j) { return j["status"] == "failed" ? j["error"]["code"].get<int>() : -1; }

  std::string track_id(const std::string& rel) {
    return conn->scalar_text("SELECT id FROM track WHERE path = ?", rel).value_or("");
  }
  std::string album_id(const std::string& name) {
    return conn->scalar_text("SELECT id FROM album WHERE name = ?", name).value_or("");
  }

  test::TempDir dir;
  fs::path music;
  Config cfg;
  crypto::Bytes key;
  int64_t alice_id = 0, bob_id = 0;
  std::string alice_key, bob_key;
  std::unique_ptr<db::WriteQueue> writer;
  std::unique_ptr<scanner::Scanner> scanner;
  std::unique_ptr<db::ReadPool> reads;
  std::unique_ptr<api::Services> services;
  std::unique_ptr<api::Router> router;
  std::unique_ptr<db::Connection> conn;
};

}  // namespace

// ---- envelope, formats, authentication ----

TEST_F(ApiTest, PingInAllFormats) {
  auto j = call("ping");
  EXPECT_EQ(j["status"], "ok");
  EXPECT_EQ(j["version"], "1.16.1");
  EXPECT_EQ(j["type"], "eos");
  EXPECT_EQ(j["openSubsonic"], true);

  auto xml = raw("ping.view", {{"apiKey", alice_key}});
  EXPECT_EQ(xml.content_type, "application/xml; charset=utf-8");
  EXPECT_NE(xml.body.find("status=\"ok\""), std::string::npos);

  auto jsonp = raw("ping", {{"apiKey", alice_key}, {"f", "jsonp"}, {"callback", "cb"}});
  EXPECT_TRUE(jsonp.body.starts_with("cb({"));
}

TEST_F(ApiTest, ExtensionsWithoutAuth) {
  auto r = raw("getOpenSubsonicExtensions", {{"f", "json"}});
  auto j = json::parse(r.body)["subsonic-response"];
  ASSERT_EQ(j["status"], "ok");
  std::set<std::string> names;
  for (const auto& e : j["openSubsonicExtensions"]) names.insert(e["name"]);
  EXPECT_TRUE(names.count("apiKeyAuthentication"));
  EXPECT_TRUE(names.count("formPost"));
  EXPECT_TRUE(names.count("indexBasedQueue"));
}

TEST_F(ApiTest, AuthMechanismsAndErrorCodes) {
  auto j = [&](std::vector<std::pair<std::string, std::string>> p) {
    p.emplace_back("f", "json");
    return json::parse(raw("ping", p).body)["subsonic-response"];
  };
  EXPECT_EQ(error_code(j({})), 10);
  EXPECT_EQ(error_code(j({{"apiKey", "eos_nope"}})), 44);
  EXPECT_EQ(error_code(j({{"apiKey", alice_key}, {"u", "alice"}})), 43);
  EXPECT_EQ(error_code(j({{"u", "alice"}, {"t", "x"}, {"s", "y"}, {"p", "z"}})), 43);

  auto salt = std::string("abc123");
  auto token = api::md5_hex("pw-alice" + salt);
  EXPECT_EQ(j({{"u", "alice"}, {"t", token}, {"s", salt}})["status"], "ok");
  EXPECT_EQ(error_code(j({{"u", "alice"}, {"t", api::md5_hex("wrong" + salt)}, {"s", salt}})), 40);
  EXPECT_EQ(error_code(j({{"u", "nobody"}, {"t", token}, {"s", salt}})), 40);

  // p= is off by default.
  EXPECT_EQ(error_code(j({{"u", "alice"}, {"p", "pw-alice"}})), 42);
  cfg.allow_password = true;
  EXPECT_EQ(j({{"u", "alice"}, {"p", "pw-alice"}})["status"], "ok");
  EXPECT_EQ(j({{"u", "alice"}, {"p", "enc:70772d616c696365"}})["status"], "ok");  // hex of pw-alice
  EXPECT_EQ(error_code(j({{"u", "alice"}, {"p", "nope"}})), 40);

  cfg.allow_token = false;
  EXPECT_EQ(error_code(j({{"u", "alice"}, {"t", token}, {"s", salt}})), 41);

  // X-Api-Key header works too.
  auto h = raw("ping", {{"f", "json"}}, {{"x-api-key", alice_key}});
  EXPECT_EQ(json::parse(h.body)["subsonic-response"]["status"], "ok");
}

TEST_F(ApiTest, FailedLoginsAreLimitedExceptOnTheLan) {
  const std::string outside = "203.0.113.5";
  auto ping = [&](const std::string& k, const std::string& ip) {
    return json::parse(raw("ping", {{"apiKey", k}, {"f", "json"}}, {}, ip).body)["subsonic-response"];
  };
  for (int i = 0; i < 10; ++i) EXPECT_EQ(error_code(ping("eos_wrong", outside)), 44);
  // Now even the right key is refused from that address...
  EXPECT_EQ(error_code(ping(alice_key, outside)), 44);
  // ...but not from another one, and LAN addresses are never limited.
  EXPECT_EQ(ping(alice_key, "198.51.100.1")["status"], "ok");
  for (int i = 0; i < 15; ++i) ping("eos_wrong", "192.168.1.50");
  EXPECT_EQ(ping(alice_key, "192.168.1.50")["status"], "ok");

  // Behind a trusted proxy on localhost, the forwarded address is what counts.
  auto via_proxy = [&](const std::string& k, const std::string& xff) {
    return json::parse(raw("ping", {{"apiKey", k}, {"f", "json"}}, {{"x-forwarded-for", xff}}, "127.0.0.1").body)
        ["subsonic-response"];
  };
  for (int i = 0; i < 10; ++i) via_proxy("eos_wrong", "203.0.113.77");
  EXPECT_EQ(error_code(via_proxy(alice_key, "203.0.113.77")), 44);
  EXPECT_EQ(via_proxy(alice_key, "203.0.113.78")["status"], "ok");
}

TEST_F(ApiTest, UnknownEndpointIs404) {
  auto r = raw("getNothing", {{"apiKey", alice_key}, {"f", "json"}});
  EXPECT_EQ(r.status, 404);
}

// ---- browsing ----

TEST_F(ApiTest, ArtistsIndexAndArtist) {
  auto j = call("getArtists");
  ASSERT_EQ(j["status"], "ok");
  EXPECT_EQ(j["artists"]["ignoredArticles"], "The El La Los Las Le Les");
  std::map<std::string, std::vector<std::string>> index;
  for (const auto& i : j["artists"]["index"])
    for (const auto& a : i["artist"]) index[i["name"]].push_back(a["name"]);
  EXPECT_EQ(index["A"], std::vector<std::string>{"Alice"});
  EXPECT_EQ(index["E"], std::vector<std::string>{"The Erin Band"});  // "The" is ignored
  EXPECT_FALSE(index.count("B"));  // Bob is a track artist only

  std::string alice;
  for (const auto& i : j["artists"]["index"])
    for (const auto& a : i["artist"])
      if (a["name"] == "Alice") alice = a["id"];
  auto artist = call("getArtist", {{"id", alice}})["artist"];
  EXPECT_EQ(artist["albumCount"], 1);
  ASSERT_EQ(artist["album"].size(), 1u);
  EXPECT_EQ(artist["album"][0]["name"], "Album A");
  EXPECT_EQ(error_code(call("getArtist", {{"id", "nope"}})), 70);
}

TEST_F(ApiTest, AlbumAndSongFields) {
  auto a = call("getAlbum", {{"id", album_id("Album A")}})["album"];
  EXPECT_EQ(a["name"], "Album A");
  EXPECT_EQ(a["songCount"], 2);
  EXPECT_EQ(a["year"], 2020);
  EXPECT_EQ(a["genre"], "Soundtrack");
  EXPECT_EQ(a["discTitles"][0]["title"], "Disc One");
  EXPECT_EQ(a["releaseDate"]["month"], 4);
  EXPECT_TRUE(std::string(a["coverArt"]).starts_with("al-"));
  ASSERT_EQ(a["song"].size(), 2u);
  EXPECT_EQ(a["song"][0]["title"], "Opening");
  EXPECT_EQ(a["song"][1]["title"], "Battle");

  auto s = call("getSong", {{"id", track_id("A/01.flac")}})["song"];
  EXPECT_EQ(s["album"], "Album A");
  EXPECT_EQ(s["suffix"], "flac");
  EXPECT_EQ(s["contentType"], "audio/flac");
  EXPECT_EQ(s["isDir"], false);
  EXPECT_EQ(s["discNumber"], 1);
  EXPECT_EQ(s["displayComposer"], "Carol");
  EXPECT_TRUE(std::string(s["coverArt"]).starts_with("mf-"));  // has its own picture
  bool performer = false;
  for (const auto& c : s["contributors"])
    if (c["role"] == "performer" && c["subRole"] == "piano" && c["artist"]["name"] == "Dave") performer = true;
  EXPECT_TRUE(performer);

  auto battle = call("getSong", {{"id", track_id("A/02.flac")}})["song"];
  EXPECT_EQ(battle["artists"].size(), 2u);
  EXPECT_TRUE(std::string(battle["coverArt"]).starts_with("al-"));
  EXPECT_TRUE(battle["contributors"].is_array());  // present even when empty
  EXPECT_EQ(error_code(call("getSong", {{"id", "nope"}})), 70);
}

TEST_F(ApiTest, GenresAndMusicFolders) {
  auto g = call("getGenres")["genres"]["genre"];
  ASSERT_EQ(g.size(), 2u);
  EXPECT_EQ(g[0]["value"], "Rock");
  EXPECT_EQ(g[1]["value"], "Soundtrack");
  EXPECT_EQ(g[1]["songCount"], 2);
  EXPECT_EQ(g[1]["albumCount"], 1);
  auto f = call("getMusicFolders")["musicFolders"]["musicFolder"];
  ASSERT_EQ(f.size(), 1u);
  EXPECT_EQ(f[0]["name"], "Music");
}

TEST_F(ApiTest, FolderBrowsingMirrorsArtists) {
  auto idx = call("getIndexes")["indexes"]["index"];
  ASSERT_FALSE(idx.empty());
  std::string alice = idx[0]["artist"][0]["id"];
  auto dir = call("getMusicDirectory", {{"id", alice}})["directory"];
  ASSERT_EQ(dir["child"].size(), 1u);
  EXPECT_EQ(dir["child"][0]["isDir"], true);
  auto albumdir = call("getMusicDirectory", {{"id", dir["child"][0]["id"]}})["directory"];
  EXPECT_EQ(albumdir["child"].size(), 2u);
  EXPECT_EQ(albumdir["child"][0]["isDir"], false);
}

// ---- lists and search ----

TEST_F(ApiTest, AlbumLists) {
  auto names = [&](json j) {  // by value: missing keys read as null
    std::vector<std::string> v;
    for (const auto& a : j["albumList2"]["album"]) v.push_back(a["name"]);
    return v;
  };
  EXPECT_EQ(names(call("getAlbumList2", {{"type", "alphabeticalByName"}})),
            (std::vector<std::string>{"Album A", "Album B"}));
  EXPECT_EQ(names(call("getAlbumList2", {{"type", "alphabeticalByName"}, {"size", "1"}, {"offset", "1"}})),
            std::vector<std::string>{"Album B"});
  EXPECT_EQ(names(call("getAlbumList2", {{"type", "byGenre"}, {"genre", "rock"}})),
            std::vector<std::string>{"Album B"});
  EXPECT_EQ(names(call("getAlbumList2", {{"type", "byYear"}, {"fromYear", "2000"}, {"toYear", "2030"}})),
            std::vector<std::string>{"Album A"});
  EXPECT_EQ(names(call("getAlbumList2", {{"type", "byYear"}, {"fromYear", "2030"}, {"toYear", "1990"}})),
            (std::vector<std::string>{"Album A", "Album B"}));
  EXPECT_EQ(call("getAlbumList2", {{"type", "random"}})["albumList2"]["album"].size(), 2u);
  EXPECT_TRUE(names(call("getAlbumList2", {{"type", "starred"}})).empty());
  EXPECT_EQ(error_code(call("getAlbumList2", {{"type", "bogus"}})), 0);
  EXPECT_EQ(error_code(call("getAlbumList2")), 10);
  EXPECT_EQ(call("getRandomSongs", {{"size", "2"}})["randomSongs"]["song"].size(), 2u);
  EXPECT_EQ(call("getRandomSongs", {{"genre", "Rock"}})["randomSongs"]["song"].size(), 1u);
}

TEST_F(ApiTest, Search3) {
  auto r = call("search3", {{"query", "open"}})["searchResult3"];
  ASSERT_EQ(r["song"].size(), 1u);
  EXPECT_EQ(r["song"][0]["title"], "Opening");
  // Kana and width are folded: hiragana finds katakana.
  r = call("search3", {{"query", "ばとる"}})["searchResult3"];
  ASSERT_EQ(r["song"].size(), 1u);
  EXPECT_EQ(r["song"][0]["title"], "バトルのテーマ");
  // Every word must match somewhere (title, album or artist).
  EXPECT_EQ(call("search3", {{"query", "battle alice"}})["searchResult3"]["song"].size(), 1u);
  EXPECT_FALSE(call("search3", {{"query", "battle erin"}})["searchResult3"].contains("song"));
  r = call("search3", {{"query", "album"}})["searchResult3"];
  EXPECT_EQ(r["album"].size(), 2u);
  EXPECT_EQ(r["album"][0]["name"], "Album A");
  // An empty query (or "") lists everything, which library-syncing clients rely on.
  r = call("search3", {{"query", "\"\""}, {"songCount", "1000"}})["searchResult3"];
  EXPECT_EQ(r["song"].size(), 3u);
  EXPECT_EQ(r["artist"].size(), 2u);
  r = call("search3", {{"query", ""}, {"songCount", "2"}, {"songOffset", "2"}})["searchResult3"];
  EXPECT_EQ(r["song"].size(), 1u);
}

// ---- annotations ----

TEST_F(ApiTest, StarRateAndScrobble) {
  auto song = track_id("A/01.flac");
  auto album = album_id("Album B");
  ASSERT_EQ(call("star", {{"id", song}, {"albumId", album}})["status"], "ok");
  auto s = call("getStarred2")["starred2"];
  EXPECT_EQ(s["song"].size(), 1u);
  EXPECT_EQ(s["album"].size(), 1u);
  EXPECT_TRUE(call("getSong", {{"id", song}})["song"].contains("starred"));
  call("unstar", {{"id", song}});
  EXPECT_FALSE(call("getStarred2")["starred2"].contains("song"));
  // Bob has his own annotations.
  EXPECT_FALSE(call("getStarred2", {}, &bob_key)["starred2"].contains("album"));

  call("setRating", {{"id", album}, {"rating", "4"}});
  EXPECT_EQ(call("getAlbum", {{"id", album}})["album"]["userRating"], 4);
  EXPECT_EQ(call("getAlbumList2", {{"type", "highest"}})["albumList2"]["album"][0]["id"], album);
  call("setRating", {{"id", album}, {"rating", "0"}});
  EXPECT_FALSE(call("getAlbum", {{"id", album}})["album"].contains("userRating"));
  EXPECT_EQ(error_code(call("setRating", {{"id", album}, {"rating", "9"}})), 0);

  call("scrobble", {{"id", song}, {"time", "1700000000000"}});
  call("scrobble", {{"id", song}});
  call("scrobble", {{"id", song}, {"submission", "false"}});  // now playing: not counted
  auto played = call("getSong", {{"id", song}})["song"];
  EXPECT_EQ(played["playCount"], 2);
  EXPECT_TRUE(played.contains("played"));
  EXPECT_EQ(call("getAlbumList2", {{"type", "frequent"}})["albumList2"]["album"][0]["name"], "Album A");
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM scrobble"), 2);
  EXPECT_EQ(error_code(call("scrobble", {{"id", "nope"}})), 70);
}

// ---- playlists ----

TEST_F(ApiTest, PlaylistLifecycleAndPermissions) {
  auto s1 = track_id("A/01.flac"), s2 = track_id("A/02.flac"), s3 = track_id("B/01.flac");
  auto created = call("createPlaylist", {{"name", "Mix"}, {"songId", s1}, {"songId", s2}, {"songId", s1}});
  ASSERT_EQ(created["status"], "ok");
  auto pl = created["playlist"];
  std::string id = pl["id"];
  EXPECT_EQ(pl["songCount"], 3);
  EXPECT_EQ(pl["owner"], "alice");
  EXPECT_EQ(pl["entry"].size(), 3u);

  call("updatePlaylist", {{"playlistId", id}, {"songIndexToRemove", "0"}, {"songIndexToRemove", "2"},
                          {"songIdToAdd", s3}, {"name", "Mix 2"}, {"comment", "hi"}});
  pl = call("getPlaylist", {{"id", id}})["playlist"];
  EXPECT_EQ(pl["name"], "Mix 2");
  EXPECT_EQ(pl["comment"], "hi");
  ASSERT_EQ(pl["entry"].size(), 2u);
  EXPECT_EQ(pl["entry"][0]["id"], s2);
  EXPECT_EQ(pl["entry"][1]["id"], s3);

  // Private to alice until made public; bob may read a public list but not change it.
  EXPECT_EQ(error_code(call("getPlaylist", {{"id", id}}, &bob_key)), 70);
  EXPECT_FALSE(call("getPlaylists", {}, &bob_key)["playlists"].contains("playlist"));
  call("updatePlaylist", {{"playlistId", id}, {"public", "true"}});
  EXPECT_EQ(call("getPlaylists", {}, &bob_key)["playlists"]["playlist"].size(), 1u);
  EXPECT_EQ(error_code(call("updatePlaylist", {{"playlistId", id}, {"name", "x"}}, &bob_key)), 50);
  EXPECT_EQ(error_code(call("deletePlaylist", {{"id", id}}, &bob_key)), 50);

  // createPlaylist with playlistId replaces the songs.
  pl = call("createPlaylist", {{"playlistId", id}, {"songId", s3}})["playlist"];
  EXPECT_EQ(pl["songCount"], 1);
  EXPECT_EQ(error_code(call("createPlaylist", {{"name", "bad"}, {"songId", "nope"}})), 70);

  call("deletePlaylist", {{"id", id}});
  EXPECT_EQ(error_code(call("getPlaylist", {{"id", id}})), 70);
}

// ---- play queue ----

TEST_F(ApiTest, PlayQueueBothForms) {
  auto s1 = track_id("A/01.flac"), s2 = track_id("A/02.flac");
  EXPECT_FALSE(call("getPlayQueue").contains("playQueue"));  // nothing saved yet

  call("savePlayQueue", {{"id", s1}, {"id", s2}, {"id", s1}, {"current", s1}, {"position", "12345"}});
  auto q = call("getPlayQueue")["playQueue"];
  EXPECT_EQ(q["current"], s1);
  EXPECT_EQ(q["position"], 12345);
  EXPECT_EQ(q["changedBy"], "tests");
  EXPECT_EQ(q["entry"].size(), 3u);
  // The same queue through the index-based form.
  auto qi = call("getPlayQueueByIndex")["playQueueByIndex"];
  EXPECT_EQ(qi["currentIndex"], 0);
  EXPECT_EQ(qi["entry"].size(), 3u);

  call("savePlayQueueByIndex", {{"id", s1}, {"id", s2}, {"id", s1}, {"currentIndex", "2"}, {"position", "5"}});
  qi = call("getPlayQueueByIndex")["playQueueByIndex"];
  EXPECT_EQ(qi["currentIndex"], 2);
  EXPECT_EQ(qi["position"], 5);
  // With a duplicate, savePlayQueue keeps the occurrence nearest the previous index.
  call("savePlayQueue", {{"id", s1}, {"id", s2}, {"id", s1}, {"current", s1}});
  EXPECT_EQ(call("getPlayQueueByIndex")["playQueueByIndex"]["currentIndex"], 2);

  EXPECT_EQ(error_code(call("savePlayQueueByIndex", {{"id", s1}, {"currentIndex", "1"}})), 10);
  EXPECT_EQ(error_code(call("savePlayQueueByIndex", {{"id", s1}})), 10);
  call("savePlayQueueByIndex");  // no parameters: clear
  EXPECT_FALSE(call("getPlayQueueByIndex").contains("playQueueByIndex"));
  // Bob's queue is separate.
  EXPECT_FALSE(call("getPlayQueue", {}, &bob_key).contains("playQueue"));
}

// ---- media ----

TEST_F(ApiTest, StreamAndDownloadWithRanges) {
  auto id = track_id("A/02.flac");
  auto size = static_cast<int64_t>(fs::file_size(music / "A/02.flac"));
  auto full = raw("stream", {{"apiKey", alice_key}, {"id", id}});
  EXPECT_EQ(full.status, 200);
  EXPECT_EQ(full.content_type, "audio/flac");
  ASSERT_TRUE(full.file);
  EXPECT_EQ(full.length, size);

  auto part = raw("stream", {{"apiKey", alice_key}, {"id", id}}, {{"range", "bytes=10-19"}});
  EXPECT_EQ(part.status, 206);
  EXPECT_EQ(part.offset, 10);
  EXPECT_EQ(part.length, 10);
  bool has_range = false;
  for (const auto& [k, v] : part.headers)
    if (k == "Content-Range") has_range = v == std::format("bytes 10-19/{}", size);
  EXPECT_TRUE(has_range);

  auto bad = raw("stream", {{"apiKey", alice_key}, {"id", id}}, {{"range", std::format("bytes={}-", size)}});
  EXPECT_EQ(bad.status, 416);

  auto dl = raw("download", {{"apiKey", alice_key}, {"id", id}});
  bool disposition = false;
  for (const auto& [k, v] : dl.headers)
    if (k == "Content-Disposition") disposition = v.find("filename=\"02.flac\"") != std::string::npos;
  EXPECT_TRUE(disposition);

  auto missing = raw("stream", {{"apiKey", alice_key}, {"id", "nope"}, {"f", "json"}});
  EXPECT_EQ(error_code(json::parse(missing.body)["subsonic-response"]), 70);
  // Authentication is required.
  auto anon = raw("stream", {{"id", id}, {"f", "json"}});
  EXPECT_EQ(error_code(json::parse(anon.body)["subsonic-response"]), 10);
}

TEST_F(ApiTest, CoverArt) {
  auto song = call("getSong", {{"id", track_id("A/01.flac")}})["song"];
  auto r = raw("getCoverArt", {{"apiKey", alice_key}, {"id", song["coverArt"]}});
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.content_type, "image/png");
  EXPECT_EQ(r.body, test::make_png(64, 32));

  // The album has no folder image, so the first track's picture is used; size shrinks it.
  auto album = call("getAlbum", {{"id", album_id("Album A")}})["album"];
  r = raw("getCoverArt", {{"apiKey", alice_key}, {"id", album["coverArt"]}, {"size", "16"}});
  EXPECT_EQ(r.content_type, "image/jpeg");
  EXPECT_TRUE(r.body.starts_with("\xFF\xD8"));
  bool immutable = false;
  for (const auto& [k, v] : r.headers)
    if (k == "Cache-Control") immutable = v.find("immutable") != std::string::npos;
  EXPECT_TRUE(immutable);
  EXPECT_FALSE(fs::is_empty(cfg.cache_dir() / "artwork"));

  // A cover.jpg in the album folder wins over embedded pictures.
  std::ofstream(music / "A/Cover.PNG", std::ios::binary) << test::make_png(8, 8);
  r = raw("getCoverArt", {{"apiKey", alice_key}, {"id", album_id("Album A")}});
  EXPECT_EQ(r.body, test::make_png(8, 8));

  auto none = raw("getCoverArt", {{"apiKey", alice_key}, {"id", "al-" + album_id("Album B")}, {"f", "json"}});
  EXPECT_EQ(error_code(json::parse(none.body)["subsonic-response"]), 70);
}

TEST_F(ApiTest, ScanEndpointsNeedAdminForStart) {
  auto s = call("getScanStatus")["scanStatus"];
  EXPECT_EQ(s["scanning"], false);
  EXPECT_EQ(s["count"], 3);
  EXPECT_TRUE(s.contains("lastScan"));
  EXPECT_EQ(error_code(call("startScan", {}, &bob_key)), 50);
  EXPECT_EQ(call("startScan")["scanStatus"]["scanning"], true);
}

TEST_F(ApiTest, GetUser) {
  auto u = call("getUser")["user"];
  EXPECT_EQ(u["username"], "alice");
  EXPECT_EQ(u["adminRole"], true);
  EXPECT_EQ(u["folder"], json::array({1}));
  EXPECT_EQ(error_code(call("getUser", {{"username", "alice"}}, &bob_key)), 50);
}
