// M3: transcoding, tag-derived custom data, suggestions, FTS search and virtual folders.
#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>

#include "api/subsonic/router.h"
#include "auth/users.h"
#include "core/config.h"
#include "db/migrate.h"
#include "fixtures.h"
#include "media/transcode.h"
#include "scanner/scanner.h"
#include "suggest/rules.h"

using namespace eos;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool have_ffmpeg() { return std::system("ffmpeg -version > /dev/null 2>&1") == 0; }

std::string drain(media::StreamSource& src, size_t limit = SIZE_MAX) {
  std::string out;
  src.run([&](const char* d, size_t n) {
    out.append(d, n);
    return out.size() < limit;
  });
  return out;
}

}  // namespace

// ---- transcoding decisions (no ffmpeg needed) ----

TEST(Transcode, Plan) {
  Config cfg;
  auto plan = [&](std::optional<std::string> f, std::optional<int64_t> br, int64_t off = 0) {
    return media::plan_transcode(cfg, "flac", 900, f, br, off);
  };
  EXPECT_FALSE(plan(std::nullopt, std::nullopt));
  EXPECT_FALSE(plan("raw", 128));
  EXPECT_FALSE(plan("flac", std::nullopt));
  EXPECT_FALSE(plan(std::nullopt, 1000));  // the original is under the limit
  EXPECT_FALSE(plan(std::nullopt, 0));

  auto p = plan("opus", 128, 30);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->format, "opus");
  EXPECT_EQ(p->content_type, "audio/ogg");
  EXPECT_EQ(p->bitrate_kbps, 128);
  EXPECT_EQ(p->offset_seconds, 30);

  p = plan(std::nullopt, 320);  // limit only: the default format
  ASSERT_TRUE(p);
  EXPECT_EQ(p->format, "opus");
  EXPECT_EQ(p->bitrate_kbps, 256);  // clamped to the opus range

  p = plan("mp3", std::nullopt);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->content_type, "audio/mpeg");
  EXPECT_EQ(p->bitrate_kbps, 192);

  p = plan("aac", 64);  // unsupported -> default format
  ASSERT_TRUE(p);
  EXPECT_EQ(p->format, "opus");
  EXPECT_EQ(p->bitrate_kbps, 64);

  auto args = media::ffmpeg_args(cfg, *plan("mp3", 128, 12), "/music/a b.flac");
  auto has = [&](const std::string& a) { return std::find(args.begin(), args.end(), a) != args.end(); };
  EXPECT_TRUE(has("-ss"));
  EXPECT_TRUE(has("12"));
  EXPECT_TRUE(has("/music/a b.flac"));  // one argument, no shell quoting
  EXPECT_TRUE(has("libmp3lame"));
  EXPECT_EQ(args.back(), "-");
}

TEST(Transcode, FfmpegRunsAndIsLimited) {
  if (!have_ffmpeg() || !test::have_flac_encoder()) GTEST_SKIP() << "ffmpeg or flac missing";
  test::TempDir dir;
  auto file = dir.path() / "a.flac";
  test::make_flac(file, 440, {{"TITLE", {"x"}}});
  Config cfg;
  cfg.transcode_max_concurrent = 1;
  media::Transcoder tc(cfg);

  auto opus = media::plan_transcode(cfg, "flac", 900, "opus", 96, 0);
  auto src = tc.start(*opus, file);
  ASSERT_TRUE(src);
  EXPECT_EQ(tc.running(), 1);
  EXPECT_EQ(tc.start(*opus, file), nullptr);  // over the limit
  auto out = drain(*src);
  EXPECT_TRUE(out.starts_with("OggS"));
  src.reset();
  EXPECT_EQ(tc.running(), 0);

  auto mp3 = media::plan_transcode(cfg, "flac", 900, "mp3", 128, 0);
  src = tc.start(*mp3, file);
  out = drain(*src);
  ASSERT_GE(out.size(), 2u);
  EXPECT_TRUE(out.starts_with("ID3") || (static_cast<unsigned char>(out[0]) == 0xFF));

  // A client that goes away stops ffmpeg early.
  src.reset();
  src = tc.start(*opus, file);
  ASSERT_TRUE(src);
  out = drain(*src, 1);
  EXPECT_FALSE(out.empty());
  src.reset();
  EXPECT_EQ(tc.running(), 0);
}

TEST(Suggest, BaseTitleAndDerivativeSubtype) {
  Config cfg;
  EXPECT_EQ(suggest::base_title("Theme (TV Size)", cfg), "theme");
  EXPECT_EQ(suggest::base_title("Theme -Off Vocal-", cfg), "theme");
  EXPECT_EQ(suggest::base_title("戦闘 (Piano Ver.)", cfg), "戦闘");
  EXPECT_EQ(suggest::base_title("Theme", cfg), "theme");
  EXPECT_EQ(suggest::derivative_subtype("Theme (TV Size)", cfg), "tv_size");
  EXPECT_EQ(suggest::derivative_subtype("Theme (Instrumental)", cfg), "instrumental");
  EXPECT_EQ(suggest::derivative_subtype("Theme (Off Vocal)", cfg), "off_vocal");
  EXPECT_EQ(suggest::derivative_subtype("Theme (DJ Foo Remix)", cfg), "remix");
  EXPECT_EQ(suggest::derivative_subtype("Theme", cfg), std::nullopt);
  EXPECT_EQ(suggest::derivative_subtype("Piano Ver.", cfg), std::nullopt);
}

// ---- library with custom tags ----

namespace {

class M3Test : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!test::have_flac_encoder()) GTEST_SKIP() << "flac encoder not installed";
    music = dir.path() / "music";
    cfg.data_dir = dir.path() / "data";
    cfg.libraries = {{"Music", music}};
    cfg.tags = default_custom_tags();
    fs::create_directories(cfg.data_dir);

    // A collection box: one physical album holding two works (Game X and Game Y),
    // with a virtual album for each game's soundtrack.
    auto box = [&](const char* file, int hz, test::Tags extra) {
      test::Tags tags = {{"ALBUM", {"Collection Box"}}, {"ALBUMARTIST", {"Various"}},
                         {"ARTIST", {"Orchestra"}}, {"DATE", {"2021"}}, {"COLLECTION", {"Soundtracks/Series A"}}};
      for (auto& [k, v] : extra) tags[k] = v;
      test::make_flac(music / "Box" / file, hz, tags);
    };
    box("01.flac", 440, {{"TITLE", {"Main Theme"}}, {"TRACKNUMBER", {"1"}}, {"COMPOSER", {"Sato"}},
                         {"WORK", {"Main Theme"}}, {"COLLECTION_WORK", {"Game X"}},
                         {"COLLECTION_ALBUM", {"Game X Soundtrack"}}});
    box("02.flac", 450, {{"TITLE", {"Main Theme (Piano Ver.)"}}, {"TRACKNUMBER", {"2"}}, {"COMPOSER", {"Sato"}},
                         {"WORK", {"Main Theme"}}, {"ARRANGER", {"Suzuki"}}, {"ARRANGEMENT", {"Piano Ver."}},
                         {"PERFORMER", {"Ito (piano)"}}, {"COLLECTION_WORK", {"Game X"}},
                         {"COLLECTION_ALBUM", {"Game X Soundtrack"}}});
    box("03.flac", 460, {{"TITLE", {"Main Theme (TV Size)"}}, {"TRACKNUMBER", {"3"}},
                         {"COLLECTION_WORK", {"Game X"}}});
    box("04.flac", 470, {{"TITLE", {"Battle"}}, {"TRACKNUMBER", {"4"}}, {"COMPOSER", {"Kato"}},
                         {"COLLECTION_WORK", {"Game Y"}}, {"COLLECTION_ALBUM", {"Game Y Soundtrack"}},
                         {"TITLESORT", {"ばとる"}}});
    box("05.flac", 480, {{"TITLE", {"Main Theme 〜 Battle"}}, {"TRACKNUMBER", {"5"}},
                         {"COLLECTION_WORK", {"Game X; Game Y"}}});
    // The same composition in another album, same arranger but another arrangement name.
    test::make_flac(music / "Live/01.flac", 490,
                    {{"TITLE", {"Main Theme (Piano Ver.)"}}, {"ALBUM", {"Live"}}, {"ALBUMARTIST", {"Ito"}},
                     {"ARTIST", {"Ito"}}, {"COMPOSER", {"Sato"}}, {"WORK", {"Main Theme"}}, {"ARRANGER", {"Suzuki"}},
                     {"ARRANGEMENT", {"Piano Version (Live)"}}, {"PERFORMANCE_TYPE", {"live"}},
                     {"DATE", {"2022"}}});
    // Two tracks without WORK but with the same title and composer.
    test::make_flac(music / "Other/01.flac", 500,
                    {{"TITLE", {"Field"}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Kato"}}, {"COMPOSER", {"Kato"}},
                     {"TRACKNUMBER", {"1"}}});
    test::make_flac(music / "Other/02.flac", 510,
                    {{"TITLE", {"Field (Arrange)"}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Kato"}},
                     {"COMPOSER", {"Kato"}}, {"TRACKNUMBER", {"2"}}});

    {
      db::Connection c(cfg.db_path());
      db::migrate(c);
      key = crypto::random_bytes(32);
      auto alice = auth::create_user(c, "alice", "pw", true, key).id;
      alice_key = auth::create_api_key(c, alice, "test").key;
    }
    writer = std::make_unique<db::WriteQueue>(cfg.db_path());
    scanner = std::make_unique<scanner::Scanner>(cfg, *writer, cfg.db_path());
    stats = scanner->scan(false);
    conn = std::make_unique<db::Connection>(cfg.db_path());
  }

  void start_api() {
    reads = std::make_unique<db::ReadPool>(cfg.db_path(), 2);
    services = std::make_unique<api::Services>(cfg, *reads, *writer, *scanner, key);
    router = std::make_unique<api::Router>(*services);
  }

  json call(const std::string& endpoint, std::vector<std::pair<std::string, std::string>> params = {}) {
    if (!router) start_api();
    params.emplace_back("apiKey", alice_key);
    params.emplace_back("f", "json");
    api::Request r;
    r.path = "/rest/" + endpoint;
    r.params = std::move(params);
    r.peer_ip = "127.0.0.1";
    auto body = router->handle(r).body;
    if (std::getenv("EOS_DEBUG")) std::cerr << endpoint << ": " << body.substr(0, 600) << "\n";
    return json::parse(body)["subsonic-response"];
  }

  std::string track(const std::string& rel) {
    return conn->scalar_text("SELECT id FROM track WHERE path = ?", rel).value_or("");
  }
  int64_t count(const std::string& sql) { return conn->scalar_int(sql).value_or(-1); }

  test::TempDir dir;
  fs::path music;
  Config cfg;
  crypto::Bytes key;
  std::string alice_key;
  scanner::ScanStats stats;
  std::unique_ptr<db::WriteQueue> writer;
  std::unique_ptr<scanner::Scanner> scanner;
  std::unique_ptr<db::Connection> conn;
  std::unique_ptr<db::ReadPool> reads;
  std::unique_ptr<api::Services> services;
  std::unique_ptr<api::Router> router;
};

}  // namespace

TEST_F(M3Test, WorksFromTags) {
  EXPECT_EQ(count("SELECT COUNT(*) FROM title"), 2);
  auto game_x = conn->scalar_text("SELECT id FROM title WHERE name = 'Game X'");
  ASSERT_TRUE(game_x);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track_title WHERE title_id = ?", *game_x), 4);
  // "Game X; Game Y" gives two works.
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track_title WHERE track_id = ?", track("Box/05.flac")), 2);

  // A manual link survives retagging; the tag link follows the tag.
  conn->run("INSERT INTO title (id, name, name_norm, source, created_at, updated_at) "
            "VALUES ('manual', 'Manual Work', 'manualwork', 3, 0, 0)");
  conn->run("INSERT INTO track_title (track_id, title_id, source) VALUES (?, 'manual', 3)", track("Box/04.flac"));
  test::retag_flac(music / "Box/04.flac", {{"TITLE", {"Battle"}}, {"ALBUM", {"Collection Box"}},
                                          {"ALBUMARTIST", {"Various"}}, {"DATE", {"2021"}},
                                          {"TRACKNUMBER", {"4"}}, {"COLLECTION", {"Soundtracks/Series A"}}});
  scanner->scan(false);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track_title WHERE track_id = ?", track("Box/04.flac")), 1);
  // Game Y is still used by 05.flac; the manual work stays.
  EXPECT_EQ(count("SELECT COUNT(*) FROM title"), 3);
}

TEST_F(M3Test, CompositionsAndArrangements) {
  EXPECT_EQ(count("SELECT COUNT(*) FROM composition"), 1);
  auto comp = *conn->scalar_text("SELECT id FROM composition");
  EXPECT_EQ(conn->scalar_text("SELECT a.name FROM participant p JOIN artist a ON a.id = p.artist_id "
                              "WHERE p.entity_type = 'composition' AND p.entity_id = ?", comp),
            "Sato");
  // Original (implicit), "Piano Ver." and "Piano Version (Live)".
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM arrangement WHERE composition_id = ?", comp), 3);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM arrangement WHERE composition_id = ? AND is_implicit = 1", comp), 1);
  EXPECT_EQ(conn->scalar_text("SELECT kind FROM arrangement WHERE name = 'Piano Ver.'"), "piano");
  auto piano = conn->scalar_text("SELECT arrangement_id FROM track_arrangement_eff WHERE track_id = ?", track("Box/02.flac"));
  EXPECT_EQ(piano, conn->scalar_text("SELECT id FROM arrangement WHERE name = 'Piano Ver.'"));
  EXPECT_EQ(conn->scalar_text("SELECT performance_kind FROM track WHERE path = 'Live/01.flac'"), "live");
  // Tracks without WORK belong to no composition.
  EXPECT_FALSE(conn->scalar_int("SELECT 1 FROM track_arrangement WHERE track_id = ?", track("Box/03.flac")));
}

TEST_F(M3Test, FoldersFromAlbumTags) {
  auto series = conn->scalar_text("SELECT id FROM folder WHERE name = 'Series A'");
  auto top = conn->scalar_text("SELECT id FROM folder WHERE name = 'Soundtracks'");
  ASSERT_TRUE(series && top);
  EXPECT_EQ(conn->scalar_text("SELECT parent_id FROM folder WHERE id = ?", *series), *top);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM album_folder WHERE folder_id = ?", *series), 1);
  EXPECT_EQ(count("SELECT COUNT(*) FROM album_folder"), 1);  // only Collection Box has the tag
}

TEST_F(M3Test, VirtualAlbums) {
  auto gx = conn->scalar_text("SELECT id FROM album WHERE kind = 'virtual' AND name = 'Game X Soundtrack'");
  ASSERT_TRUE(gx);
  EXPECT_EQ(conn->scalar_int("SELECT song_count FROM album WHERE id = ?", *gx), 2);
  EXPECT_EQ(count("SELECT COUNT(*) FROM album WHERE kind = 'virtual'"), 2);
  // The tracks keep their physical album.
  EXPECT_EQ(conn->scalar_text("SELECT a.name FROM track t JOIN album a ON a.id = t.album_id WHERE t.path = 'Box/01.flac'"),
            "Collection Box");

  // Hidden from /rest lists by default; getAlbum works on the id.
  auto names = [&](json j) {
    std::set<std::string> s;
    for (const auto& a : j["albumList2"]["album"]) s.insert(a["name"]);
    return s;
  };
  EXPECT_FALSE(names(call("getAlbumList2", {{"type", "alphabeticalByName"}, {"size", "50"}})).count("Game X Soundtrack"));
  auto album = call("getAlbum", {{"id", *gx}})["album"];
  ASSERT_EQ(album["song"].size(), 2u);
  EXPECT_EQ(album["song"][0]["title"], "Main Theme");
  EXPECT_EQ(album["song"][0]["albumId"], conn->scalar_text("SELECT id FROM album WHERE name = 'Collection Box'"));

  cfg.show_virtual_albums = true;
  EXPECT_TRUE(names(call("getAlbumList2", {{"type", "alphabeticalByName"}, {"size", "50"}})).count("Game X Soundtrack"));

  // Dropping the tag removes the tag-made virtual album.
  for (auto [file, title, n] : {std::tuple{"Box/04.flac", "Battle", "4"}})
    test::retag_flac(music / file, {{"TITLE", {title}}, {"ALBUM", {"Collection Box"}}, {"ALBUMARTIST", {"Various"}},
                                    {"DATE", {"2021"}}, {"TRACKNUMBER", {n}}});
  scanner->scan(false);
  EXPECT_FALSE(conn->scalar_int("SELECT 1 FROM album WHERE name = 'Game Y Soundtrack'"));
}

TEST_F(M3Test, RuleSuggestions) {
  EXPECT_GT(stats.suggestions, 0);
  auto rules = [&] {
    std::map<std::string, int> m;
    auto st = conn->prepare("SELECT rule, COUNT(*) FROM suggestion WHERE status = 'pending' GROUP BY rule");
    while (st.step()) m[st.text(0)] = static_cast<int>(st.int64(1));
    return m;
  };
  auto r = rules();
  EXPECT_EQ(r["derivative"], 1);           // Main Theme (TV Size) -> Main Theme
  EXPECT_EQ(r["medley"], 1);               // Main Theme 〜 Battle
  EXPECT_EQ(r["same_composition"], 1);     // Field / Field (Arrange), composer Kato
  EXPECT_EQ(r["performance_variant"], 1);  // two piano arrangements by Suzuki

  auto payload = [&](const char* rule) {
    return json::parse(*conn->scalar_text("SELECT payload FROM suggestion WHERE rule = ?", rule));
  };
  auto d = payload("derivative");
  EXPECT_EQ(d["subtype"], "tv_size");
  EXPECT_EQ(d["from"]["id"], track("Box/03.flac"));
  EXPECT_EQ(d["to"]["id"], track("Box/01.flac"));
  auto m = payload("medley");
  ASSERT_EQ(m["parts"].size(), 2u);
  EXPECT_EQ(m["parts"][0]["to"]["type"], "composition");
  EXPECT_EQ(m["parts"][1]["to"]["id"], track("Box/04.flac"));
  EXPECT_EQ(payload("same_composition")["tracks"].size(), 2u);

  // A rejected suggestion is not raised again; a rescan adds nothing new.
  conn->run("UPDATE suggestion SET status = 'rejected' WHERE rule = 'derivative'");
  auto again = scanner->scan(true);
  EXPECT_EQ(again.suggestions, 0);
  EXPECT_EQ(rules()["derivative"], 0);
  EXPECT_EQ(count("SELECT COUNT(*) FROM suggestion WHERE rule = 'derivative'"), 1);

  // Once the tags say the same, the suggestion goes stale.
  for (auto [file, title, n] : {std::tuple{"Other/01.flac", "Field", "1"}, {"Other/02.flac", "Field (Arrange)", "2"}})
    test::retag_flac(music / file, {{"TITLE", {title}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Kato"}},
                                    {"COMPOSER", {"Kato"}}, {"TRACKNUMBER", {n}}, {"WORK", {"Field"}}});
  scanner->scan(false);
  EXPECT_EQ(conn->scalar_text("SELECT status FROM suggestion WHERE rule = 'same_composition'"), "stale");
}

TEST_F(M3Test, FtsSearch) {
  auto songs = [&](const std::string& q) {
    std::vector<std::string> v;
    auto res = call("search3", {{"query", q}, {"artistCount", "0"}, {"albumCount", "0"}});
    for (const auto& s : res["searchResult3"]["song"]) v.push_back(s["title"]);
    return v;
  };
  EXPECT_GT(count("SELECT COUNT(*) FROM search_index"), 10);
  // Exact title first, then prefix, then the rest.
  auto hits = songs("main theme");
  ASSERT_GE(hits.size(), 4u);
  EXPECT_EQ(hits[0], "Main Theme");
  EXPECT_EQ(songs("ばとる").front(), "Battle");       // the reading from TITLESORT
  EXPECT_EQ(songs("suzuki").size(), 0u);              // arrangers are not indexed on songs
  EXPECT_EQ(songs("sato").size(), 3u);                // composers are (display_composer)
  EXPECT_EQ(songs("ld").size(), 2u);                  // under 3 characters: substring fallback
  EXPECT_EQ(songs("field kato").size(), 2u);
  auto albums = call("search3", {{"query", "box"}})["searchResult3"]["album"];
  ASSERT_EQ(albums.size(), 1u);
  EXPECT_EQ(albums[0]["name"], "Collection Box");
}

TEST_F(M3Test, FolderBrowsing) {
  auto idx = call("getIndexes")["indexes"]["index"];
  std::map<std::string, std::string> entries;
  for (const auto& i : idx)
    for (const auto& a : i["artist"]) entries[a["name"]] = a["id"];
  ASSERT_TRUE(entries.count("Soundtracks"));
  ASSERT_TRUE(entries.count("（未分類）"));

  auto top = call("getMusicDirectory", {{"id", entries["Soundtracks"]}})["directory"];
  ASSERT_EQ(top["child"].size(), 1u);
  EXPECT_EQ(top["child"][0]["title"], "Series A");
  auto series = call("getMusicDirectory", {{"id", top["child"][0]["id"]}})["directory"];
  EXPECT_EQ(series["parent"], entries["Soundtracks"]);
  ASSERT_EQ(series["child"].size(), 1u);
  EXPECT_EQ(series["child"][0]["title"], "Collection Box");
  auto album = call("getMusicDirectory", {{"id", series["child"][0]["id"]}})["directory"];
  EXPECT_EQ(album["parent"], top["child"][0]["id"]);
  EXPECT_EQ(album["child"].size(), 5u);

  // Unfiled holds the albums without a folder tag, virtual ones included.
  std::set<std::string> unfiled;
  auto unfiled_dir = call("getMusicDirectory", {{"id", "fo-unfiled"}});
  for (const auto& c : unfiled_dir["directory"]["child"]) unfiled.insert(c["title"]);
  EXPECT_TRUE(unfiled.count("Live"));
  EXPECT_TRUE(unfiled.count("Other"));
  EXPECT_TRUE(unfiled.count("Game X Soundtrack"));
  EXPECT_FALSE(unfiled.count("Collection Box"));
}

TEST_F(M3Test, StreamTranscodesWhenAsked) {
  start_api();
  api::Request r;
  r.path = "/rest/stream";
  r.params = {{"apiKey", alice_key}, {"id", track("Box/01.flac")}, {"format", "opus"}, {"maxBitRate", "96"}};
  r.peer_ip = "127.0.0.1";
  auto resp = router->handle(r);
  EXPECT_EQ(resp.content_type, "audio/ogg");
  ASSERT_TRUE(resp.stream);
  EXPECT_FALSE(resp.file);
  if (have_ffmpeg()) EXPECT_TRUE(drain(*resp.stream).starts_with("OggS"));
  resp.stream.reset();

  r.params = {{"apiKey", alice_key}, {"id", track("Box/01.flac")}, {"format", "raw"}};
  resp = router->handle(r);
  EXPECT_TRUE(resp.file);
  EXPECT_FALSE(resp.stream);

  auto ext = call("getOpenSubsonicExtensions");
  bool offset = false;
  for (const auto& e : ext["openSubsonicExtensions"]) offset = offset || e["name"] == "transcodeOffset";
  EXPECT_TRUE(offset);
}

TEST_F(M3Test, UpgradeForcesOneFullRescan) {
  conn->run("INSERT OR REPLACE INTO setting (key, value) VALUES ('rescan_required', '1')");
  auto s = scanner->scan(false);
  EXPECT_EQ(s.updated, 8);
  EXPECT_EQ(scanner->scan(false).unchanged, 8);
}
