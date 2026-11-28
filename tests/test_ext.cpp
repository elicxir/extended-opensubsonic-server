// M4: the /ext API, end to end through ExtRouter (no network).
#include <fstream>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>

#include "api/ext/router.h"
#include "auth/users.h"
#include "core/config.h"
#include "db/migrate.h"
#include "fixtures.h"
#include "scanner/scanner.h"

using namespace eos;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Resp {
  int status;
  json body;
};

// A library with works, folders, compositions and virtual albums (as in the M3 tests).
void make_library(const fs::path& music) {
  auto box = [&](const char* file, int hz, test::Tags extra) {
    test::Tags tags = {{"ALBUM", {"Collection Box"}}, {"ALBUMARTIST", {"Various"}}, {"ARTIST", {"Orchestra"}},
                       {"DATE", {"2021"}}, {"COLLECTION", {"Soundtracks/Series A"}}};
    for (auto& [k, v] : extra) tags[k] = v;
    test::make_flac(music / "Box" / file, hz, tags);
  };
  box("01.flac", 440, {{"TITLE", {"Main Theme"}}, {"TRACKNUMBER", {"1"}}, {"COMPOSER", {"Sato"}},
                       {"WORK", {"Main Theme"}}, {"COLLECTION_WORK", {"Game X"}},
                       {"COLLECTION_ALBUM", {"Game X Soundtrack"}}});
  box("02.flac", 450, {{"TITLE", {"Main Theme (Piano Ver.)"}}, {"TRACKNUMBER", {"2"}}, {"COMPOSER", {"Sato"}},
                       {"WORK", {"Main Theme"}}, {"ARRANGER", {"Suzuki"}}, {"ARRANGEMENT", {"Piano Ver."}},
                       {"COLLECTION_WORK", {"Game X"}}, {"COLLECTION_ALBUM", {"Game X Soundtrack"}}});
  box("03.flac", 460, {{"TITLE", {"Main Theme (TV Size)"}}, {"TRACKNUMBER", {"3"}}, {"COLLECTION_WORK", {"Game X"}}});
  box("04.flac", 470, {{"TITLE", {"Battle"}}, {"TRACKNUMBER", {"4"}}, {"COMPOSER", {"Kato"}},
                       {"COLLECTION_WORK", {"Game Y"}}});
  box("05.flac", 480, {{"TITLE", {"Main Theme 〜 Battle (Medley)"}}, {"TRACKNUMBER", {"5"}},
                       {"COLLECTION_WORK", {"Game X; Game Y"}}});
  test::make_flac(music / "Other/01.flac", 500,
                  {{"TITLE", {"Field"}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Kato"}}, {"COMPOSER", {"Kato"}},
                   {"TRACKNUMBER", {"1"}}});
  test::make_flac(music / "Other/02.flac", 510,
                  {{"TITLE", {"Field (Arrange)"}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Kato"}}, {"COMPOSER", {"Kato"}},
                   {"TRACKNUMBER", {"2"}}});
}

// One server instance: config, database, scanner, services and the /ext router.
struct Server {
  explicit Server(const fs::path& root, const fs::path& music) {
    cfg.data_dir = root;
    cfg.libraries = {{"Music", music}};
    cfg.tags = default_custom_tags();
    fs::create_directories(cfg.data_dir);
    {
      db::Connection c(cfg.db_path());
      db::migrate(c);
      key = crypto::random_bytes(32);
      admin_key = auth::create_api_key(c, auth::create_user(c, "alice", "pw-alice", true, key).id, "test").key;
      user_key = auth::create_api_key(c, auth::create_user(c, "bob", "pw-bob", false, key).id, "test").key;
    }
    writer = std::make_unique<db::WriteQueue>(cfg.db_path());
    scanner = std::make_unique<scanner::Scanner>(cfg, *writer, cfg.db_path());
    scanner->scan(false);
    reads = std::make_unique<db::ReadPool>(cfg.db_path(), 2);
    services = std::make_unique<api::Services>(cfg, *reads, *writer, *scanner, key);
    router = std::make_unique<api::ext::ExtRouter>(*services);
    conn = std::make_unique<db::Connection>(cfg.db_path());
  }

  Resp call(const std::string& method, const std::string& path, const json& body = nullptr,
            const std::string* as = nullptr, std::vector<std::pair<std::string, std::string>> params = {}) {
    api::Request r;
    r.method = method;
    auto q = path.find('?');
    r.path = path.substr(0, q);
    if (q != std::string::npos) api::parse_query(path.substr(q + 1), r.params);
    for (auto& p : params) r.params.push_back(std::move(p));
    r.headers["x-api-key"] = as ? *as : admin_key;
    if (!body.is_null()) r.body = body.dump();
    r.peer_ip = "127.0.0.1";
    auto resp = router->handle(r);
    return {resp.status, resp.body.empty() ? json(nullptr) : json::parse(resp.body)};
  }
  Resp get(const std::string& path, const std::string* as = nullptr) { return call("GET", path, nullptr, as); }

  std::string track(const std::string& rel) {
    return conn->scalar_text("SELECT id FROM track WHERE path = ?", rel).value_or("");
  }
  std::string album(const std::string& name) {
    return conn->scalar_text("SELECT id FROM album WHERE name = ?", name).value_or("");
  }

  Config cfg;
  crypto::Bytes key;
  std::string admin_key, user_key;
  std::unique_ptr<db::WriteQueue> writer;
  std::unique_ptr<scanner::Scanner> scanner;
  std::unique_ptr<db::ReadPool> reads;
  std::unique_ptr<api::Services> services;
  std::unique_ptr<api::ext::ExtRouter> router;
  std::unique_ptr<db::Connection> conn;
};

class ExtTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!test::have_flac_encoder()) GTEST_SKIP() << "flac encoder not installed";
    music = dir.path() / "music";
    make_library(music);
    s = std::make_unique<Server>(dir.path() / "data", music);
  }
  test::TempDir dir;
  fs::path music;
  std::unique_ptr<Server> s;
};

std::set<std::string> titles_of(const json& songs) {
  std::set<std::string> out;
  for (const auto& x : songs) out.insert(x["title"]);
  return out;
}

}  // namespace

TEST_F(ExtTest, AuthAndErrors) {
  api::Request r;
  r.method = "GET";
  r.path = "/ext/info";
  r.peer_ip = "127.0.0.1";
  auto anon = s->router->handle(r);
  EXPECT_EQ(anon.status, 401);
  EXPECT_EQ(json::parse(anon.body)["error"]["code"], "unauthorized");
  r.params = {{"apiKey", s->admin_key}};
  EXPECT_EQ(s->router->handle(r).status, 200);  // query auth works too

  EXPECT_EQ(s->get("/ext/info").body["apiVersion"], 1);
  EXPECT_EQ(s->get("/ext/nothing").status, 404);
  EXPECT_EQ(s->call("DELETE", "/ext/info").status, 405);
  EXPECT_EQ(s->call("POST", "/ext/titles", nullptr).status, 400);  // body required
  auto bad = s->call("POST", "/ext/titles", json{{"name", "Work"}}, &s->user_key);
  EXPECT_EQ(bad.status, 403);
  EXPECT_EQ(bad.body["error"]["code"], "forbidden");
  EXPECT_EQ(s->get("/ext/scan").body["last"]["added"], 7);
}

TEST_F(ExtTest, FoldersAlbumsAndTitles) {
  auto folders = s->get("/ext/folders").body;
  ASSERT_EQ(folders["total"], 2);
  std::string series;
  for (const auto& f : folders["items"])
    if (f["name"] == "Series A") series = f["id"];
  auto detail = s->get("/ext/folders/" + series).body;
  EXPECT_EQ(detail["folder"]["albumCount"], 1);
  EXPECT_EQ(detail["albums"][0]["name"], "Collection Box");

  EXPECT_EQ(s->get("/ext/albums?kind=virtual").body["total"], 1);
  EXPECT_EQ(s->get("/ext/albums?kind=physical").body["total"], 2);
  EXPECT_EQ(s->get("/ext/albums?kind=bogus").status, 400);
  auto box = s->get("/ext/albums/" + s->album("Collection Box")).body;
  EXPECT_EQ(box["kind"], "physical");
  EXPECT_EQ(box["song"].size(), 5u);
  EXPECT_EQ(box["virtualAlbums"][0]["name"], "Game X Soundtrack");
  EXPECT_EQ(box["folderIds"][0], series);
  auto vx = s->get("/ext/albums/" + s->album("Game X Soundtrack")).body;
  EXPECT_EQ(vx["kind"], "virtual");
  EXPECT_EQ(vx["parentAlbums"][0]["name"], "Collection Box");

  // Songs grouped by work; the medley belongs to both works.
  auto groups = s->get("/ext/albums/" + s->album("Collection Box") + "/titles").body["groups"];
  ASSERT_EQ(groups.size(), 2u);
  EXPECT_EQ(groups[0]["title"]["name"], "Game X");
  EXPECT_EQ(groups[0]["songs"].size(), 4u);
  EXPECT_EQ(groups[1]["title"]["name"], "Game Y");
  EXPECT_EQ(titles_of(groups[1]["songs"]), (std::set<std::string>{"Battle", "Main Theme 〜 Battle (Medley)"}));

  auto titles = s->get("/ext/titles?query=game").body;
  EXPECT_EQ(titles["total"], 2);
  std::string game_x = titles["items"][0]["id"];
  EXPECT_EQ(titles["items"][0]["songCount"], 4);
  auto tx = s->get("/ext/titles/" + game_x).body;
  EXPECT_EQ(tx["title"]["provenance"], "tag");
  EXPECT_EQ(tx["songs"].size(), 4u);
}

TEST_F(ExtTest, CompositionsAndVersions) {
  auto comps = s->get("/ext/compositions").body;
  ASSERT_EQ(comps["total"], 1);
  std::string comp = comps["items"][0]["id"];
  auto c = s->get("/ext/compositions/" + comp).body;
  EXPECT_EQ(c["composition"]["composers"][0]["name"], "Sato");
  ASSERT_EQ(c["arrangements"].size(), 2u);
  EXPECT_EQ(c["arrangements"][0]["arrangement"]["isImplicit"], true);
  EXPECT_EQ(c["arrangements"][1]["arrangement"]["arrangers"][0]["name"], "Suzuki");

  auto v = s->get("/ext/songs/" + s->track("Box/01.flac") + "/versions").body;
  EXPECT_EQ(v["composition"]["id"], comp);
  EXPECT_EQ(v["song"]["arrangementProvenance"], "tag");
  EXPECT_EQ(v["song"]["variationCount"], 1);  // the piano version
  EXPECT_EQ(v["arrangements"][0]["songs"][0]["variationCount"], 1);
  EXPECT_EQ(s->get("/ext/songs/" + s->track("Box/04.flac")).body["variationCount"], 0);
  ASSERT_EQ(v["arrangements"].size(), 1u);
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["name"], "Piano Ver.");
  EXPECT_EQ(v["arrangements"][0]["songs"][0]["title"], "Main Theme (Piano Ver.)");
  EXPECT_TRUE(v["performances"].empty());
  EXPECT_TRUE(v["related"].empty());
  EXPECT_EQ(s->get("/ext/songs/nope/versions").status, 404);
}

TEST_F(ExtTest, SongDetail) {
  auto d = s->get("/ext/songs/" + s->track("Box/02.flac") + "/detail").body;
  EXPECT_EQ(d["song"]["title"], "Main Theme (Piano Ver.)");
  EXPECT_EQ(d["album"]["name"], "Collection Box");
  EXPECT_EQ(d["album"]["kind"], "physical");
  EXPECT_EQ(d["composition"]["name"], "Main Theme");
  EXPECT_EQ(d["composition"]["composers"][0]["name"], "Sato");
  EXPECT_EQ(d["arrangement"]["name"], "Piano Ver.");
  EXPECT_EQ(d["arrangement"]["arrangers"][0]["name"], "Suzuki");
  EXPECT_EQ(d["arrangementLink"]["provenance"], "tag");
  EXPECT_EQ(d["arrangementLink"]["origin"], "tag");
  ASSERT_EQ(d["titles"].size(), 1u);
  EXPECT_EQ(d["titles"][0]["title"]["name"], "Game X");
  EXPECT_EQ(d["titles"][0]["link"]["provenance"], "tag");
  ASSERT_EQ(d["virtualAlbums"].size(), 1u);
  EXPECT_EQ(d["virtualAlbums"][0]["album"]["name"], "Game X Soundtrack");
  EXPECT_EQ(d["virtualAlbums"][0]["album"]["kind"], "virtual");
  EXPECT_EQ(d["virtualAlbums"][0]["link"]["provenance"], "tag");
  EXPECT_TRUE(d["virtualAlbums"][0].contains("discNumber"));
  EXPECT_EQ(d["virtualAlbums"][0]["trackNumber"], 2);
  EXPECT_EQ(d["file"]["library"], "Music");
  EXPECT_EQ(d["file"]["path"], "Box/02.flac");
  EXPECT_GT(d["file"]["size"].get<int64_t>(), 0);
  EXPECT_EQ(d["file"]["modified"].get<std::string>().size(), 24u);
  EXPECT_TRUE(d["file"]["audioMd5"].is_string());
  EXPECT_EQ(d["tags"]["ARRANGEMENT"], json::array({"Piano Ver."}));
  EXPECT_EQ(d["tags"]["COLLECTION_WORK"], json::array({"Game X"}));

  // A song nothing is known about: the links are null or empty, never missing.
  auto plain = s->get("/ext/songs/" + s->track("Other/01.flac") + "/detail").body;
  EXPECT_TRUE(plain["titles"].empty());
  EXPECT_TRUE(plain["virtualAlbums"].empty());
  EXPECT_TRUE(plain.contains("composition") && plain.contains("arrangement") && plain.contains("arrangementLink"));

  // An estimated link says how sure it is and what it rests on; includeEstimated=false hides it.
  auto pending = s->get("/ext/suggestions?rule=same_composition").body["items"];
  ASSERT_FALSE(pending.empty());
  std::string sid = pending[0]["id"];
  ASSERT_EQ(s->call("POST", "/ext/suggestions/" + sid + "/accept").status, 200);
  const std::string member = s->track("Other/02.flac");
  auto est = s->get("/ext/songs/" + member + "/detail").body;
  EXPECT_EQ(est["composition"]["name"], "Field");
  EXPECT_EQ(est["arrangementLink"]["provenance"], "rule");
  EXPECT_EQ(est["arrangementLink"]["rule"], "same_composition");
  EXPECT_EQ(est["arrangementLink"]["origin"], "name");
  EXPECT_TRUE(est["arrangementLink"]["confidence"].is_number());
  auto sure = s->get("/ext/songs/" + member + "/detail?includeEstimated=false").body;
  EXPECT_TRUE(sure["arrangementLink"].is_null());
  EXPECT_TRUE(sure["composition"].is_null());
  EXPECT_EQ(s->get("/ext/songs/nope/detail").status, 404);
  auto features = s->get("/ext/info").body["features"];
  EXPECT_NE(std::find(features.begin(), features.end(), "songDetail"), features.end());
}

TEST_F(ExtTest, AcceptAndRejectSuggestions) {
  auto list = s->get("/ext/suggestions").body;
  EXPECT_EQ(list["total"], 3);  // derivative, medley, same_composition
  std::map<std::string, json> by_rule;
  for (const auto& x : list["items"]) by_rule[x["rule"]] = x;
  ASSERT_TRUE(by_rule.count("derivative"));
  EXPECT_FALSE(by_rule["derivative"]["summary"].get<std::string>().empty());
  EXPECT_EQ(by_rule["derivative"]["targets"].size(), 2u);

  // Bob cannot decide.
  EXPECT_EQ(s->call("POST", "/ext/suggestions/" + by_rule["derivative"]["id"].get<std::string>() + "/accept", nullptr,
                    &s->user_key).status,
            403);

  auto acc = s->call("POST", "/ext/suggestions/" + by_rule["derivative"]["id"].get<std::string>() + "/accept");
  ASSERT_EQ(acc.status, 200);
  EXPECT_EQ(acc.body["status"], "accepted");
  auto v = s->get("/ext/songs/" + s->track("Box/03.flac") + "/versions").body;
  ASSERT_EQ(v["related"].size(), 1u);
  EXPECT_EQ(v["related"][0]["relation"]["type"], "derivative");
  EXPECT_EQ(v["related"][0]["relation"]["subtype"], "tv_size");
  EXPECT_EQ(v["related"][0]["relation"]["provenance"], "rule");
  EXPECT_EQ(v["related"][0]["relation"]["direction"], "out");
  EXPECT_EQ(v["related"][0]["target"]["song"]["title"], "Main Theme");
  // Accepting twice is a conflict.
  EXPECT_EQ(s->call("POST", "/ext/suggestions/" + by_rule["derivative"]["id"].get<std::string>() + "/accept").status, 409);

  // Accept with edits: written as manual data.
  auto edited = s->call("POST", "/ext/suggestions/" + by_rule["same_composition"]["id"].get<std::string>() + "/accept",
                        json{{"edits", {{"name", "Field Theme"}}}});
  ASSERT_EQ(edited.status, 200);
  auto field = s->get("/ext/songs/" + s->track("Other/02.flac") + "/versions").body;
  EXPECT_EQ(field["composition"]["name"], "Field Theme");
  EXPECT_EQ(field["composition"]["provenance"], "manual");
  EXPECT_EQ(field["performances"][0]["title"], "Field");

  auto batch = s->call("POST", "/ext/suggestions/batch", json{{"reject", {by_rule["medley"]["id"]}}});
  EXPECT_EQ(batch.body["rejected"], 1);
  EXPECT_EQ(s->get("/ext/suggestions").body["total"], 0);
  EXPECT_EQ(s->get("/ext/suggestions?status=rejected").body["total"], 1);
  // A rescan neither revives the rejected one nor re-raises the accepted ones.
  s->scanner->scan(true);
  EXPECT_EQ(s->get("/ext/suggestions").body["total"], 0);
}

TEST_F(ExtTest, LlmEstimatesWithAutoApply) {
  s->cfg.estimate_auto_apply_min_confidence = 0.9;
  auto battle = s->track("Box/04.flac");
  auto field = s->track("Other/01.flac");
  json body = {{"suggestions",
                {{{"rule", "title"},
                  {"model", "test-model"},
                  {"score", 0.95},
                  {"evidence", {{"reason", "VGMdb lists it"}, {"urls", {"https://example.invalid/x"}}}},
                  {"proposal", {{"action", "link_title"}, {"titleName", "Game Z"}, {"tracks", {field}}}}},
                 {{"rule", "relation"},
                  {"score", 0.5},
                  {"proposal",
                   {{"action", "relation"},
                    {"type", "quote"},
                    {"from", {{"type", "track"}, {"id", battle}}},
                    {"to", {{"type", "track"}, {"id", field}}}}}},
                 {{"rule", "bad"}, {"score", 0.99}, {"proposal", {{"action", "link_title"}, {"tracks", {"nope"}}}}}}}};
  auto r = s->call("POST", "/ext/suggestions", body);
  ASSERT_EQ(r.status, 201);
  EXPECT_EQ(r.body["created"].size(), 2u);
  EXPECT_EQ(r.body["autoApplied"], 1);
  ASSERT_EQ(r.body["errors"].size(), 1u);
  EXPECT_EQ(r.body["errors"][0]["index"], 2);
  EXPECT_FALSE(s->conn->scalar_int("SELECT 1 FROM title WHERE name = 'nope'"));

  // The confident one is applied as an estimate; it is visible unless estimates are hidden.
  auto song = s->get("/ext/songs/" + field).body;
  ASSERT_EQ(song["titleIds"].size(), 1u);
  auto z = s->get("/ext/titles/" + song["titleIds"][0].get<std::string>()).body;
  EXPECT_EQ(z["title"]["name"], "Game Z");
  EXPECT_EQ(s->get("/ext/songs/" + field + "?includeEstimated=false").body["titleIds"].size(), 0u);

  auto pending = s->get("/ext/suggestions?method=llm").body;
  ASSERT_EQ(pending["total"], 1);
  EXPECT_EQ(pending["items"][0]["score"], 0.5);
  EXPECT_EQ(s->get("/ext/suggestions?status=auto_applied").body["items"][0]["evidence"]["reason"], "VGMdb lists it");

  // The same estimate again is skipped.
  EXPECT_EQ(s->call("POST", "/ext/suggestions", body).body["skipped"], 2);
}

TEST_F(ExtTest, LinkArrangementProposal) {
  // What a MusicBrainz import proposes: "Field (Arrange)" is an arrangement of "Field".
  auto f1 = s->track("Other/01.flac"), f2 = s->track("Other/02.flac");
  json body = {{"suggestions",
                {{{"rule", "musicbrainz"}, {"score", 0.95},
                  {"proposal", {{"action", "link_arrangement"},
                                {"composition", {{"name", "Field"}, {"composers", {"Kato"}},
                                                 {"musicBrainzWorkId", "00000000-0000-4000-8000-000000000001"}}},
                                {"tracks", {f1}}}}},
                 {{"rule", "musicbrainz"}, {"score", 0.95},
                  {"proposal", {{"action", "link_arrangement"},
                                {"composition", {{"name", "Field"}, {"musicBrainzWorkId", "00000000-0000-4000-8000-000000000001"}}},
                                {"arrangement", {{"name", "Field (Arrange)"}, {"arrangers", {"Ito"}}}},
                                {"tracks", {f2}}}}}}}};
  auto r = s->call("POST", "/ext/suggestions", body);
  ASSERT_EQ(r.status, 201);
  ASSERT_EQ(r.body["created"].size(), 2u);
  EXPECT_TRUE(r.body["errors"].empty());
  auto list = s->get("/ext/suggestions?method=llm").body;
  for (const auto& sug : list["items"])
    ASSERT_EQ(s->call("POST", "/ext/suggestions/" + sug["id"].get<std::string>() + "/accept").status, 200);
  auto v = s->get("/ext/songs/" + f1 + "/versions").body;
  EXPECT_EQ(v["composition"]["name"], "Field");
  EXPECT_EQ(v["composition"]["musicBrainzWorkId"], "00000000-0000-4000-8000-000000000001");
  EXPECT_EQ(v["song"]["arrangementProvenance"], "llm");
  ASSERT_EQ(v["arrangements"].size(), 1u);
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["name"], "Field (Arrange)");
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["arrangers"][0]["name"], "Ito");
  EXPECT_EQ(v["arrangements"][0]["songs"][0]["id"], f2);
}

TEST_F(ExtTest, LinkArrangementWithOriginalTracks) {
  // One suggestion says "Field (Arrange) arranges Field": both land in the same composition.
  auto f1 = s->track("Other/01.flac"), f2 = s->track("Other/02.flac");
  json body = {{"suggestions",
                {{{"rule", "audio_match"}, {"score", 0.9},
                  {"proposal", {{"action", "link_arrangement"},
                                {"composition", {{"name", "Field"}, {"composers", {"Kato"}}}},
                                {"arrangement", {{"name", "Piano Album"}}},
                                {"tracks", {f2}},
                                {"originalTracks", {f1}}}}}}}};
  auto created = s->call("POST", "/ext/suggestions", body);
  ASSERT_EQ(created.status, 201);
  ASSERT_EQ(created.body["created"].size(), 1u) << created.body.dump();
  auto id = created.body["created"][0].get<std::string>();
  auto sug = s->get("/ext/suggestions/" + id).body;
  EXPECT_EQ(sug["targets"].size(), 2u);
  EXPECT_NE(sug["summary"].get<std::string>().find("1曲を原曲に"), std::string::npos) << sug["summary"];
  ASSERT_EQ(s->call("POST", "/ext/suggestions/" + id + "/accept").status, 200);
  auto v = s->get("/ext/songs/" + f1 + "/versions").body;
  EXPECT_EQ(v["composition"]["name"], "Field");
  ASSERT_EQ(v["arrangements"].size(), 1u);
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["name"], "Piano Album");
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["kind"], "piano");
  EXPECT_EQ(v["arrangements"][0]["songs"][0]["id"], f2);
}

TEST_F(ExtTest, LinkedTracksWithArrangerGoToTheirArrangement) {
  // COMPOSER is the original composer and ARRANGER the arranger, but there is no WORK tag:
  // a proposal that names no arrangement puts such tracks in the arrangers' arrangement.
  test::make_flac(music / "Other/03.flac", 520,
                  {{"TITLE", {"Field (Arcade)"}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Ito"}}, {"COMPOSER", {"Kato"}},
                   {"ARRANGER", {"Ito"}}, {"TRACKNUMBER", {"3"}}});
  test::make_flac(music / "Other/04.flac", 530,
                  {{"TITLE", {"Field (Live)"}}, {"ALBUM", {"Other"}}, {"ARTIST", {"Ito"}}, {"COMPOSER", {"Kato"}},
                   {"ARRANGER", {"Ito"}}, {"TRACKNUMBER", {"4"}}});
  s->scanner->scan(true);
  auto f1 = s->track("Other/01.flac"), f3 = s->track("Other/03.flac"), f4 = s->track("Other/04.flac");
  json body = {{"suggestions",
                {{{"rule", "musicbrainz_alias"}, {"score", 0.85},
                  {"proposal", {{"action", "link_arrangement"},
                                {"composition", {{"name", "Field"}, {"composers", {"Kato"}}}},
                                {"tracks", {f1, f3, f4}}}}}}}};
  auto created = s->call("POST", "/ext/suggestions", body);
  ASSERT_EQ(created.status, 201);
  ASSERT_EQ(created.body["created"].size(), 1u) << created.body.dump();
  auto accepted = s->call("POST", "/ext/suggestions/" + created.body["created"][0].get<std::string>() + "/accept");
  ASSERT_EQ(accepted.status, 200) << accepted.body.dump();
  auto v = s->get("/ext/songs/" + f1 + "/versions").body;
  EXPECT_EQ(v["composition"]["name"], "Field") << v.dump();
  ASSERT_EQ(v["arrangements"].size(), 1u);  // one arrangement by Ito, holding both tracks
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["name"], nullptr);
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["arrangers"][0]["name"], "Ito");
  EXPECT_EQ(titles_of(v["arrangements"][0]["songs"]), (std::set<std::string>{"Field (Arcade)", "Field (Live)"}));
  EXPECT_EQ(s->get("/ext/songs/" + f3).body["arrangementId"],
            v["arrangements"][0]["arrangement"]["id"]);
}

TEST_F(ExtTest, ManualCompositionsArrangementsAndRelations) {
  auto comp = s->call("POST", "/ext/compositions", json{{"name", "Field Theme"}, {"composers", {"Kato"}}});
  ASSERT_EQ(comp.status, 201);
  std::string comp_id = comp.body["id"];
  EXPECT_EQ(comp.body["provenance"], "manual");
  auto arr = s->call("POST", "/ext/compositions/" + comp_id + "/arrangements",
                     json{{"name", "Arrange Ver."}, {"arrangers", {"Ito"}}});
  ASSERT_EQ(arr.status, 201);
  auto f2 = s->track("Other/02.flac");
  auto linked = s->call("PUT", "/ext/songs/" + f2 + "/arrangement", json{{"arrangementId", arr.body["id"]}});
  EXPECT_EQ(linked.body["compositionId"], comp_id);
  EXPECT_EQ(linked.body["arrangementProvenance"], "manual");
  EXPECT_EQ(s->call("DELETE", "/ext/songs/" + f2 + "/arrangement").body["compositionId"], nullptr);

  // Tag-made rows cannot be deleted.
  std::string tag_comp = s->get("/ext/songs/" + s->track("Box/01.flac")).body["compositionId"];
  auto del = s->call("DELETE", "/ext/compositions/" + tag_comp);
  EXPECT_EQ(del.status, 409);
  EXPECT_EQ(del.body["error"]["code"], "tag_managed");
  EXPECT_EQ(s->call("DELETE", "/ext/compositions/" + comp_id).status, 204);

  auto rel = s->call("POST", "/ext/relations",
                     json{{"from", {{"type", "track"}, {"id", s->track("Box/04.flac")}}},
                          {"to", {{"type", "composition"}, {"id", tag_comp}}},
                          {"type", "quote"},
                          {"note", "motif at 1:20"}});
  ASSERT_EQ(rel.status, 201);
  EXPECT_EQ(s->call("POST", "/ext/relations",
                    json{{"from", {{"type", "track"}, {"id", s->track("Box/04.flac")}}},
                         {"to", {{"type", "composition"}, {"id", tag_comp}}},
                         {"type", "quote"}}).status,
            409);
  EXPECT_EQ(s->call("POST", "/ext/relations", json{{"from", {{"type", "track"}, {"id", "x"}}},
                                                   {"to", {{"type", "track"}, {"id", "y"}}},
                                                   {"type", "quote"}}).status,
            404);
  auto v = s->get("/ext/songs/" + s->track("Box/01.flac") + "/versions").body;
  ASSERT_EQ(v["related"].size(), 1u);  // via the composition
  EXPECT_EQ(v["related"][0]["relation"]["direction"], "in");
  EXPECT_EQ(v["related"][0]["target"]["song"]["title"], "Battle");
  auto upd = s->call("PUT", "/ext/relations/" + rel.body["id"].get<std::string>(), json{{"note", nullptr}, {"position", 2}});
  EXPECT_EQ(upd.body["note"], nullptr);
  EXPECT_EQ(upd.body["position"], 2);
  EXPECT_EQ(s->call("DELETE", "/ext/relations/" + rel.body["id"].get<std::string>()).status, 204);
}

TEST_F(ExtTest, TitlesFoldersAndExclusions) {
  auto t = s->call("POST", "/ext/titles", json{{"name", "Game W"}, {"kind", "game"}, {"year", 2001}});
  ASSERT_EQ(t.status, 201);
  EXPECT_EQ(s->call("POST", "/ext/titles", json{{"name", "game  w"}}).status, 409);
  std::string game_x = s->get("/ext/titles?query=game x").body["items"][0]["id"];
  auto song = s->track("Box/01.flac");
  auto patched = s->call("PATCH", "/ext/songs/" + song + "/titles", json{{"add", {t.body["id"]}}, {"remove", {game_x}}});
  EXPECT_EQ(patched.body["titleIds"], json::array({t.body["id"]}));
  // The removed tag link is an exclusion that survives rescans.
  s->scanner->scan(true);
  EXPECT_EQ(s->get("/ext/songs/" + song).body["titleIds"], json::array({t.body["id"]}));
  EXPECT_EQ(s->call("DELETE", "/ext/titles/" + game_x).status, 409);
  EXPECT_EQ(s->call("DELETE", "/ext/titles/" + t.body["id"].get<std::string>()).status, 204);

  auto top = s->call("POST", "/ext/folders", json{{"name", "Favourites"}});
  ASSERT_EQ(top.status, 201);
  auto sub = s->call("POST", "/ext/folders", json{{"name", "Piano"}, {"parentId", top.body["id"]}});
  ASSERT_EQ(sub.status, 201);
  EXPECT_EQ(s->call("PUT", "/ext/folders/" + top.body["id"].get<std::string>(), json{{"name", "Best"}}).body["name"], "Best");
  EXPECT_EQ(s->call("PUT", "/ext/folders/" + top.body["id"].get<std::string>(), json{{"parentId", sub.body["id"]}}).status,
            400);  // into its own child
  std::string series;
  auto all_folders = s->get("/ext/folders").body;
  for (const auto& f : all_folders["items"])
    if (f["name"] == "Series A") series = f["id"];
  ASSERT_FALSE(series.empty());
  EXPECT_EQ(s->call("PUT", "/ext/folders/" + series, json{{"name", "x"}}).status, 409);
  EXPECT_EQ(s->call("PUT", "/ext/folders/" + series, json{{"sortOrder", 3}}).body["sortOrder"], 3);
  auto af = s->call("PATCH", "/ext/albums/" + s->album("Other") + "/folders", json{{"add", {sub.body["id"]}}});
  EXPECT_EQ(af.body["folderIds"], json::array({sub.body["id"]}));
  EXPECT_EQ(s->get("/ext/folders/" + sub.body["id"].get<std::string>()).body["albums"][0]["name"], "Other");
}

TEST_F(ExtTest, VirtualAlbumsAndOverrides) {
  auto f1 = s->track("Other/01.flac"), f2 = s->track("Other/02.flac");
  auto va = s->call("POST", "/ext/albums", json{{"name", "Field Suite"}, {"albumArtist", "Kato"}, {"tracks", {f2, f1}}});
  ASSERT_EQ(va.status, 201);
  std::string va_id = va.body["id"];
  EXPECT_EQ(va.body["kind"], "virtual");
  EXPECT_EQ(va.body["songCount"], 2);
  auto detail = s->get("/ext/albums/" + va_id).body;
  EXPECT_EQ(detail["song"][0]["title"], "Field (Arrange)");  // the album's own order
  auto patched = s->call("PATCH", "/ext/albums/" + va_id + "/tracks", json{{"remove", {f1}}});
  EXPECT_EQ(patched.body["songCount"], 1);
  EXPECT_EQ(s->call("PATCH", "/ext/albums/" + s->album("Other") + "/tracks", json{{"add", {f1}}}).status, 409);
  EXPECT_EQ(s->call("DELETE", "/ext/albums/" + s->album("Other")).status, 409);
  EXPECT_EQ(s->call("DELETE", "/ext/albums/" + s->album("Game X Soundtrack")).status, 409);
  EXPECT_EQ(s->call("DELETE", "/ext/albums/" + va_id).status, 204);

  // Overrides beat tags, survive rescans, show up in search, and null restores the tag.
  auto song = s->call("PATCH", "/ext/songs/" + f1, json{{"title", "Open Field"}, {"sortTitle", "おーぷん"}});
  EXPECT_EQ(song.body["title"], "Open Field");
  s->scanner->scan(true);
  EXPECT_EQ(s->get("/ext/songs/" + f1).body["title"], "Open Field");
  auto found = s->get("/ext/search?q=open field").body;
  EXPECT_EQ(found["songs"][0]["title"], "Open Field");
  EXPECT_EQ(s->call("PATCH", "/ext/songs/" + f1, json{{"title", nullptr}}).body["title"], "Field");
  auto album = s->call("PATCH", "/ext/albums/" + s->album("Other"), json{{"name", "Other Album"}});
  EXPECT_EQ(album.body["name"], "Other Album");
  EXPECT_EQ(s->call("PATCH", "/ext/albums/" + album.body["id"].get<std::string>(), json{{"name", nullptr}}).body["name"],
            "Other");
}

TEST_F(ExtTest, SearchGroupsByCompositionAndWork) {
  auto r = s->get("/ext/search?q=main theme").body;
  ASSERT_EQ(r["compositions"].size(), 1u);
  EXPECT_EQ(titles_of(r["compositions"][0]["songs"]),
            (std::set<std::string>{"Main Theme", "Main Theme (Piano Ver.)"}));
  // Songs already listed under the composition are not repeated.
  EXPECT_EQ(titles_of(r["songs"]), (std::set<std::string>{"Main Theme (TV Size)", "Main Theme 〜 Battle (Medley)"}));
  r = s->get("/ext/search?q=game y").body;
  ASSERT_EQ(r["titles"].size(), 1u);
  EXPECT_EQ(r["titles"][0]["songCount"], 2);
  EXPECT_TRUE(s->get("/ext/search").body["songs"].empty());
}

TEST_F(ExtTest, PreferredVersionsPerUser) {
  std::string comp = s->get("/ext/compositions").body["items"][0]["id"];
  auto piano = s->track("Box/02.flac");
  EXPECT_EQ(s->call("PUT", "/ext/compositions/" + comp + "/preferred", json{{"songId", piano}}, &s->user_key).status, 200);
  EXPECT_EQ(s->get("/ext/preferred", &s->user_key).body["items"][0]["songId"], piano);
  EXPECT_EQ(s->get("/ext/preferred").body["total"], 0);  // alice has none
  EXPECT_EQ(s->call("DELETE", "/ext/compositions/" + comp + "/preferred", nullptr, &s->user_key).status, 204);
}

TEST_F(ExtTest, LoginAndApiKeys) {
  api::Request r;
  r.method = "POST";
  r.path = "/ext/auth/login";
  r.peer_ip = "203.0.113.4";
  r.body = json{{"username", "bob"}, {"password", "wrong"}, {"deviceName", "pi"}}.dump();
  EXPECT_EQ(s->router->handle(r).status, 401);
  r.body = json{{"username", "bob"}, {"password", "pw-bob"}, {"deviceName", "pi"}}.dump();
  auto ok = s->router->handle(r);
  ASSERT_EQ(ok.status, 201);
  std::string key = json::parse(ok.body)["key"];
  EXPECT_EQ(s->get("/ext/info", &key).status, 200);

  auto keys = s->get("/ext/apikeys", &key).body;
  EXPECT_EQ(keys["total"], 2);  // "test" and "pi"
  auto made = s->call("POST", "/ext/apikeys", json{{"deviceName", "desktop"}}, &key);
  ASSERT_EQ(made.status, 201);
  std::string desktop_id = made.body["id"];
  // Alice (admin) may revoke it; bob may not revoke alice's keys.
  std::string alice_key_id = s->get("/ext/apikeys").body["items"][0]["id"];
  EXPECT_EQ(s->call("DELETE", "/ext/apikeys/" + alice_key_id, nullptr, &key).status, 404);
  EXPECT_EQ(s->call("DELETE", "/ext/apikeys/" + desktop_id, nullptr, &key).status, 204);
  std::string desktop_key = made.body["key"];
  EXPECT_EQ(s->get("/ext/info", &desktop_key).status, 401);
}

TEST_F(ExtTest, ExportAndImportIntoFreshDatabase) {
  // Manual data of several kinds.
  auto f1 = s->track("Other/01.flac");
  auto title = s->call("POST", "/ext/titles", json{{"name", "Game W"}}).body;
  s->call("PATCH", "/ext/songs/" + f1 + "/titles", json{{"add", {title["id"]}}});
  s->call("PATCH", "/ext/songs/" + f1, json{{"title", "Open Field"}});
  std::string tag_comp = s->get("/ext/songs/" + s->track("Box/01.flac")).body["compositionId"];
  s->call("POST", "/ext/relations", json{{"from", {{"type", "track"}, {"id", s->track("Box/04.flac")}}},
                                         {"to", {{"type", "composition"}, {"id", tag_comp}}},
                                         {"type", "quote"}});
  auto folder = s->call("POST", "/ext/folders", json{{"name", "Favourites"}}).body;
  s->call("PATCH", "/ext/albums/" + s->album("Other") + "/folders", json{{"add", {folder["id"]}}});
  auto rejected = s->get("/ext/suggestions").body["items"][0]["id"].get<std::string>();
  s->call("POST", "/ext/suggestions/" + rejected + "/reject");

  auto exported = s->get("/ext/export");
  ASSERT_EQ(exported.status, 200);
  EXPECT_EQ(exported.body["format"], "eos-export");
  EXPECT_EQ(s->get("/ext/export", &s->user_key).status, 403);

  // A brand-new server over the same music (ids are rebuilt by scanning).
  Server fresh(dir.path() / "data2", music);
  auto imported = fresh.call("POST", "/ext/import", exported.body);
  ASSERT_EQ(imported.status, 200);
  EXPECT_EQ(imported.body["unmatched"]["tracks"], 0);
  auto song = fresh.get("/ext/songs/" + fresh.track("Other/01.flac")).body;
  EXPECT_EQ(song["title"], "Open Field");
  ASSERT_EQ(song["titleIds"].size(), 1u);
  EXPECT_EQ(fresh.get("/ext/titles/" + song["titleIds"][0].get<std::string>()).body["title"]["name"], "Game W");
  auto v = fresh.get("/ext/songs/" + fresh.track("Box/04.flac") + "/versions").body;
  EXPECT_EQ(v["related"].size(), 1u);
  EXPECT_EQ(fresh.get("/ext/albums/" + fresh.album("Other")).body["folderIds"].size(), 1u);
  EXPECT_EQ(fresh.get("/ext/suggestions?status=rejected").body["total"], 1);
  // The rejected suggestion keeps its fingerprint, so the next scan does not raise it again.
  fresh.scanner->scan(true);
  EXPECT_EQ(fresh.get("/ext/suggestions?status=rejected").body["total"], 1);
  // (How many stay pending depends on the imported overrides: "Open Field" no longer
  // pairs with "Field (Arrange)", so that suggestion went stale.)
  // Importing twice adds nothing new.
  auto again = fresh.call("POST", "/ext/import", exported.body);
  EXPECT_FALSE(again.body["imported"].contains("relations"));
}

namespace {

const json* find_node(const json& g, const std::string& id) {
  for (const auto& n : g["nodes"])
    if (n["id"] == id) return &n;
  return nullptr;
}

size_t count_edges(const json& g, const std::string& type) {
  size_t n = 0;
  for (const auto& e : g["edges"]) n += e["type"] == type;
  return n;
}

}  // namespace

TEST_F(ExtTest, GraphAroundASong) {
  auto t1 = s->track("Box/01.flac"), t2 = s->track("Box/02.flac"), t4 = s->track("Box/04.flac"),
       t5 = s->track("Box/05.flac");
  std::string comp = s->get("/ext/songs/" + t1).body["compositionId"];

  // Two steps: the composition and the work, then the other songs they hold.
  auto r = s->get("/ext/graph?seed=song:" + t1, &s->user_key);
  ASSERT_EQ(r.status, 200) << r.body.dump();
  auto g = r.body;
  EXPECT_EQ(g["seed"], "song:" + t1);
  EXPECT_EQ(g["truncated"], false);
  EXPECT_EQ(g["nodes"].size(), 6u);  // 01, composition, Game X, 02, 03, 05
  EXPECT_EQ(g["nodes"][0]["id"], "song:" + t1);
  EXPECT_EQ(g["nodes"][0]["label"], "Main Theme");
  EXPECT_EQ(g["nodes"][0]["distance"], 0);
  EXPECT_EQ(g["nodes"][0]["song"]["compositionId"], comp);
  EXPECT_EQ(count_edges(g, "performance"), 2u);
  EXPECT_EQ(count_edges(g, "title"), 4u);
  EXPECT_EQ(g["edges"].size(), 6u);
  auto* c = find_node(g, "composition:" + comp);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ((*c)["type"], "composition");
  EXPECT_EQ((*c)["label"], "Main Theme");
  EXPECT_EQ((*c)["degree"], 2);
  EXPECT_EQ((*c)["expanded"], true);
  EXPECT_EQ((*c)["composition"]["id"], comp);
  // The medley is also in Game Y, which is three steps away.
  auto* medley = find_node(g, "song:" + t5);
  ASSERT_NE(medley, nullptr);
  EXPECT_EQ((*medley)["distance"], 2);
  EXPECT_EQ((*medley)["degree"], 2);
  EXPECT_EQ((*medley)["expanded"], false);
  for (const auto& e : g["edges"]) {
    if (e["type"] != "performance" || e["from"] != "song:" + t2) continue;
    EXPECT_EQ(e["to"], "composition:" + comp);
    EXPECT_EQ(e["arrangementName"], "Piano Ver.");
    EXPECT_EQ(e["arrangementKind"], "piano");
    EXPECT_EQ(e["isOriginal"], false);
    EXPECT_EQ(e["provenance"], "tag");
  }

  // One step only: the hubs are shown but not opened.
  auto g1 = s->get("/ext/graph?seed=track:" + t1 + "&depth=1").body;
  EXPECT_EQ(g1["nodes"].size(), 3u);
  EXPECT_EQ(g1["edges"].size(), 2u);
  EXPECT_EQ((*find_node(g1, "composition:" + comp))["expanded"], false);

  // A relation joins a song to the composition, with a direction.
  auto rel = s->call("POST", "/ext/relations",
                     json{{"from", {{"type", "track"}, {"id", t4}}},
                          {"to", {{"type", "composition"}, {"id", comp}}},
                          {"type", "quote"}});
  ASSERT_EQ(rel.status, 201);
  g = s->get("/ext/graph?seed=song:" + t1).body;
  EXPECT_EQ(g["nodes"].size(), 7u);
  ASSERT_EQ(count_edges(g, "quote"), 1u);
  for (const auto& e : g["edges"]) {
    if (e["type"] != "quote") continue;
    EXPECT_EQ(e["from"], "song:" + t4);
    EXPECT_EQ(e["to"], "composition:" + comp);
    EXPECT_EQ(e["directed"], true);
    EXPECT_EQ(e["relationId"], rel.body["id"]);
    EXPECT_EQ(e["provenance"], "manual");
  }
  EXPECT_EQ(count_edges(s->get("/ext/graph?seed=song:" + t1 + "&include=composition,title").body, "quote"), 0u);

  // The node budget keeps the nearest nodes.
  auto small = s->get("/ext/graph?seed=song:" + t1 + "&limit=3").body;
  EXPECT_EQ(small["nodes"].size(), 3u);
  EXPECT_EQ(small["truncated"], true);

  // Any node can be the centre.
  auto from_comp = s->get("/ext/graph?seed=composition:" + comp + "&depth=1").body;
  EXPECT_EQ(from_comp["nodes"].size(), 4u);  // composition, 01, 02, 04
  EXPECT_EQ(from_comp["edges"].size(), 3u);

  EXPECT_EQ(s->get("/ext/graph").status, 400);
  EXPECT_EQ(s->get("/ext/graph?seed=song:nope").status, 404);
  EXPECT_EQ(s->get("/ext/graph?seed=folder:1").status, 400);
  EXPECT_EQ(s->get("/ext/graph?seed=song:" + t1 + "&include=bogus").status, 400);
  EXPECT_EQ(s->get("/ext/graph?seed=song:" + t1 + "&depth=two").status, 400);
}

TEST_F(ExtTest, GraphPeopleAndAlbumsOnRequest) {
  auto kato = s->conn->scalar_text("SELECT id FROM artist WHERE name = 'Kato'").value_or("");
  ASSERT_FALSE(kato.empty());
  auto o1 = s->track("Other/01.flac");

  // People are not followed unless asked for.
  EXPECT_EQ(s->get("/ext/graph?seed=song:" + o1).body["nodes"].size(), 1u);

  // Kato composed Battle and both Field tracks, and performs the Field tracks.
  auto g = s->get("/ext/graph?seed=artist:" + kato + "&include=artist&depth=1").body;
  EXPECT_EQ(g["nodes"].size(), 4u);
  EXPECT_EQ(g["nodes"][0]["label"], "Kato");
  EXPECT_EQ(count_edges(g, "credit"), 5u);
  g = s->get("/ext/graph?seed=artist:" + kato + "&include=artist&depth=1&roles=composer").body;
  EXPECT_EQ(g["edges"].size(), 3u);
  for (const auto& e : g["edges"]) {
    EXPECT_EQ(e["role"], "composer");
    EXPECT_EQ(e["to"], "artist:" + kato);
  }

  // From a song, a person leads to their other songs...
  g = s->get("/ext/graph?seed=song:" + o1 + "&include=artist").body;
  EXPECT_EQ(g["nodes"].size(), 4u);  // Field, Kato, Battle, Field (Arrange)
  EXPECT_EQ((*find_node(g, "artist:" + kato))["expanded"], true);
  // ...unless they have too many: then the node is shown with its size and left closed.
  g = s->get("/ext/graph?seed=song:" + o1 + "&include=artist&fanout=2").body;
  EXPECT_EQ(g["nodes"].size(), 2u);
  EXPECT_EQ((*find_node(g, "artist:" + kato))["degree"], 3);
  EXPECT_EQ((*find_node(g, "artist:" + kato))["expanded"], false);
  EXPECT_EQ(g["truncated"], false);

  // Albums: the physical album and the virtual albums of a song.
  auto box = s->album("Collection Box");
  g = s->get("/ext/graph?seed=album:" + box + "&include=album&depth=1").body;
  EXPECT_EQ(g["nodes"].size(), 6u);
  EXPECT_EQ(g["nodes"][0]["album"]["kind"], "physical");
  EXPECT_EQ(g["edges"].size(), 5u);
  auto* first = find_node(g, "song:" + s->track("Box/01.flac"));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ((*first)["degree"], 2);  // also in the virtual album "Game X Soundtrack"
  EXPECT_EQ((*first)["expanded"], false);
}

TEST_F(ExtTest, SimilarityReplacedOneKindAtATime) {
  auto t1 = s->track("Box/01.flac"), t2 = s->track("Box/02.flac"), t4 = s->track("Box/04.flac"),
       f1 = s->track("Other/01.flac");
  ASSERT_FALSE(t1.empty());
  auto info = s->get("/ext/info").body;
  EXPECT_NE(std::find(info["features"].begin(), info["features"].end(), "similarity"), info["features"].end());
  EXPECT_EQ(s->get("/ext/similarity").body["total"], 0);

  // Either order; a pair given twice keeps the higher score; a song with itself and unknown
  // ids are dropped (the unknown one is reported).
  json body = {{"model", "test-model"},
               {"params", {{"k", 2}, {"centered", true}}},
               {"trackCount", 7},
               {"pairs", json::array({json::array({t2, t1, 0.5}), json::array({t1, t2, 0.75}),
                                      json::array({t1, t4, 0.25}), json::array({t1, t1, 1.0}),
                                      json::array({t1, "nosuchsong", 0.9})})}};
  auto r = s->call("PUT", "/ext/similarity/audio_embed", body);
  ASSERT_EQ(r.status, 200) << r.body.dump();
  EXPECT_EQ(r.body["pairCount"], 2);
  EXPECT_EQ(r.body["skipped"], 1);
  auto pair_score = [&](const std::string& kind, std::string a, std::string b) {
    if (b < a) std::swap(a, b);
    auto st = s->conn->prepare("SELECT score FROM similarity WHERE kind = ? AND track_a = ? AND track_b = ?");
    st.bind_all(kind, a, b);
    return st.step() ? st.real(0) : -1.0;
  };
  EXPECT_DOUBLE_EQ(pair_score("audio_embed", t1, t2), 0.75);
  EXPECT_DOUBLE_EQ(pair_score("audio_embed", t4, t1), 0.25);

  auto sets = s->get("/ext/similarity").body;
  ASSERT_EQ(sets["total"], 1);
  EXPECT_EQ(sets["items"][0]["kind"], "audio_embed");
  EXPECT_EQ(sets["items"][0]["model"], "test-model");
  EXPECT_EQ(sets["items"][0]["params"]["k"], 2);
  EXPECT_EQ(sets["items"][0]["trackCount"], 7);
  EXPECT_EQ(sets["items"][0]["pairCount"], 2);

  // Another kind lives beside it; replacing a kind drops its old pairs only.
  ASSERT_EQ(s->call("PUT", "/ext/similarity/audio_match",
                    json{{"model", "m2"}, {"pairs", json::array({json::array({t1, f1, 0.1})})}})
                .status,
            200);
  ASSERT_EQ(s->call("PUT", "/ext/similarity/audio_embed",
                    json{{"model", "test-model"}, {"pairs", json::array({json::array({t2, t4, 0.5})})}})
                .status,
            200);
  EXPECT_EQ(pair_score("audio_embed", t1, t2), -1.0);
  EXPECT_DOUBLE_EQ(pair_score("audio_embed", t2, t4), 0.5);
  EXPECT_DOUBLE_EQ(pair_score("audio_match", t1, f1), 0.1);
  sets = s->get("/ext/similarity").body;
  ASSERT_EQ(sets["total"], 2);
  EXPECT_EQ(sets["items"][0]["kind"], "audio_embed");
  EXPECT_EQ(sets["items"][0]["trackCount"], 2);  // no trackCount: the songs that have a pair

  // A purged song takes its pairs with it.
  s->conn->exec("PRAGMA foreign_keys = ON");
  s->conn->run("DELETE FROM track WHERE id = ?", f1);
  EXPECT_EQ(s->conn->scalar_int("SELECT COUNT(*) FROM similarity WHERE kind = 'audio_match'"), 0);

  EXPECT_EQ(s->call("DELETE", "/ext/similarity/audio_match").status, 204);
  EXPECT_EQ(s->call("DELETE", "/ext/similarity/audio_match").status, 404);
  EXPECT_EQ(s->get("/ext/similarity").body["total"], 1);
}

TEST_F(ExtTest, SimilarityIsForAdministratorsAndChecksItsInput) {
  auto t1 = s->track("Box/01.flac"), t2 = s->track("Box/02.flac");
  json good = {{"model", "m"}, {"pairs", json::array({json::array({t1, t2, 0.5})})}};
  EXPECT_EQ(s->call("PUT", "/ext/similarity/audio_embed", good, &s->user_key).status, 403);
  EXPECT_EQ(s->get("/ext/similarity", &s->user_key).status, 403);
  EXPECT_EQ(s->call("DELETE", "/ext/similarity/audio_embed", nullptr, &s->user_key).status, 403);

  EXPECT_EQ(s->call("PUT", "/ext/similarity/Audio-Embed", good).status, 400);
  EXPECT_EQ(s->call("PUT", "/ext/similarity/audio_embed", json{{"pairs", json::array()}}).status, 400);
  EXPECT_EQ(s->call("PUT", "/ext/similarity/audio_embed", json{{"model", "m"}}).status, 400);
  EXPECT_EQ(s->call("PUT", "/ext/similarity/audio_embed",
                    json{{"model", "m"}, {"pairs", json::array({json::array({t1, 0.5})})}})
                .status,
            400);
  EXPECT_EQ(s->call("PUT", "/ext/similarity/audio_embed", json{{"model", "m"}, {"params", 3}, {"pairs", json::array()}})
                .status,
            400);
  // A bad request changes nothing.
  EXPECT_EQ(s->get("/ext/similarity").body["total"], 0);
  // An empty set is allowed (a kind with no pairs yet).
  auto r = s->call("PUT", "/ext/similarity/audio_embed", json{{"model", "m"}, {"pairs", json::array()}});
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body["pairCount"], 0);
}

TEST_F(ExtTest, GraphSimilarFromTheSeedOnly) {
  auto t1 = s->track("Box/01.flac"), t2 = s->track("Box/02.flac"), t4 = s->track("Box/04.flac"),
       t5 = s->track("Box/05.flac"), f1 = s->track("Other/01.flac"), f2 = s->track("Other/02.flac");
  const auto seed = "/ext/graph?seed=song:" + t1 + "&include=similar";
  // No similarity stored yet: asking for it is an error, not an empty graph.
  EXPECT_EQ(s->get(seed).status, 400);

  json body = {{"model", "test-model"},
               {"pairs", json::array({json::array({t1, f1, 0.9}), json::array({t1, f2, 0.8}),
                                      json::array({t1, t2, 0.95}), json::array({t1, t4, 0.3}),
                                      json::array({f1, f2, 0.7}), json::array({f1, t5, 0.85})})}};
  ASSERT_EQ(s->call("PUT", "/ext/similarity/audio_embed", body).status, 200);

  // Default: the 5 best from other albums (Box/02 and Box/04 share the seed's album). The
  // neighbours are not walked further: Box/05 is only similar to Other/01.
  auto r = s->get(seed + "&depth=3", &s->user_key);
  ASSERT_EQ(r.status, 200) << r.body.dump();
  auto g = r.body;
  EXPECT_EQ(g["nodes"].size(), 3u);
  EXPECT_EQ(g["nodes"][1]["id"], "song:" + f1);  // best first
  EXPECT_EQ(g["nodes"][2]["id"], "song:" + f2);
  EXPECT_EQ(find_node(g, "song:" + t5), nullptr);
  EXPECT_EQ(count_edges(g, "similar"), 2u);  // Other/01 and /02 share an album
  EXPECT_EQ((*find_node(g, "song:" + t1))["expanded"], true);
  EXPECT_EQ((*find_node(g, "song:" + f1))["expanded"], false);
  EXPECT_EQ((*find_node(g, "song:" + f1))["degree"], 2);  // the seed and Box/05
  for (const auto& e : g["edges"]) {
    if (e["from"] != "song:" + t1 || e["to"] != "song:" + f1) continue;
    EXPECT_EQ(e["type"], "similar");
    EXPECT_EQ(e["kind"], "audio_embed");
    EXPECT_DOUBLE_EQ(e["score"].get<double>(), 0.9);
    EXPECT_EQ(e["provenance"], "computed");
    EXPECT_FALSE(e.contains("directed"));
  }

  // The same album too: the edge between two neighbours (Other/01 and /02) is shown as well.
  g = s->get(seed + "&similarOtherAlbums=false").body;
  EXPECT_EQ(g["nodes"].size(), 5u);
  EXPECT_EQ(g["nodes"][1]["id"], "song:" + t2);
  EXPECT_EQ(count_edges(g, "similar"), 5u);
  // k and a lowest score.
  g = s->get(seed + "&similarK=1").body;
  ASSERT_EQ(g["nodes"].size(), 2u);
  EXPECT_EQ(g["nodes"][1]["id"], "song:" + f1);
  g = s->get(seed + "&similarMinScore=0.85").body;
  EXPECT_EQ(g["nodes"].size(), 2u);

  // Not asked for: no similar edges.
  EXPECT_EQ(count_edges(s->get("/ext/graph?seed=song:" + t1).body, "similar"), 0u);

  EXPECT_EQ(s->get(seed + "&similarKind=audio_match").status, 400);
  EXPECT_EQ(s->get(seed + "&similarMinScore=high").status, 400);
  EXPECT_EQ(s->get(seed + "&similarK=x").status, 400);
}

namespace {

// Field / Field (Arrange) by Kato: the rules propose them as one composition by title.
// mb_link() sends what mb-match would: the same songs under a MusicBrainz work.
struct Approvals {
  Server& s;
  std::string f1 = s.track("Other/01.flac"), f2 = s.track("Other/02.flac");

  std::string title_match() {
    auto items = s.get("/ext/suggestions?rule=same_composition").body["items"];
    EXPECT_EQ(items.size(), 1u);
    return items.empty() ? "" : items[0]["id"].get<std::string>();
  }
  std::string mb_link() {
    json body = {{"suggestions",
                  json::array({{{"rule", "mb_match"},
                                {"score", 0.8},
                                {"origin", "external"},
                                {"originRef", "https://musicbrainz.org/work/mbid-field"},
                                {"proposal",
                                 {{"action", "link_arrangement"},
                                  {"composition", {{"name", "Field Theme"}, {"musicBrainzWorkId", "mbid-field"},
                                                   {"composers", {"Kato"}}}},
                                  {"tracks", {f1, f2}}}}}})}};
    auto r = s.call("POST", "/ext/suggestions", body);
    EXPECT_EQ(r.status, 201) << r.body.dump();
    return r.body["created"][0];
  }
  int accept(const std::string& id) { return s.call("POST", "/ext/suggestions/" + id + "/accept").status; }
  std::string composition_of(const std::string& track) {
    auto v = s.get("/ext/songs/" + track).body["compositionId"];
    return v.is_string() ? v.get<std::string>() : "";
  }
  int64_t count(const std::string& sql) { return s.conn->scalar_int(sql).value_or(0); }
};

}  // namespace

TEST_F(ExtTest, TitleMatchDoesNotUndoMusicBrainz) {
  Approvals a{*s};
  auto title = a.title_match(), mb = a.mb_link();
  ASSERT_EQ(a.accept(mb), 200);
  auto work = a.composition_of(a.f1);
  ASSERT_FALSE(work.empty());
  // Accepting the older title match afterwards must not pull the songs out again.
  ASSERT_EQ(a.accept(title), 200);
  EXPECT_EQ(a.composition_of(a.f1), work);
  EXPECT_EQ(a.composition_of(a.f2), work);
  EXPECT_EQ(a.count("SELECT COUNT(*) FROM composition WHERE name_norm = 'field'"), 0);
  EXPECT_EQ(a.count("SELECT COUNT(*) FROM composition"),
            a.count("SELECT COUNT(DISTINCT a.composition_id) FROM track_arrangement ta "
                    "JOIN arrangement a ON a.id = ta.arrangement_id"));
}

TEST_F(ExtTest, MovedSongsLeaveNoEmptyComposition) {
  Approvals a{*s};
  auto title = a.title_match(), mb = a.mb_link();
  ASSERT_EQ(a.accept(title), 200);
  auto by_title = a.composition_of(a.f1);
  ASSERT_FALSE(by_title.empty());
  // MusicBrainz moves both songs on: the title-made composition is left empty and goes.
  ASSERT_EQ(a.accept(mb), 200);
  auto work = a.composition_of(a.f1);
  EXPECT_NE(work, by_title);
  EXPECT_EQ(a.composition_of(a.f2), work);
  EXPECT_EQ(s->get("/ext/compositions/" + by_title).status, 404);
  EXPECT_EQ(a.count("SELECT COUNT(*) FROM arrangement WHERE composition_id = '" + by_title + "'"), 0);
  EXPECT_EQ(a.count("SELECT COUNT(*) FROM participant WHERE entity_type = 'composition' AND entity_id = '" +
                    by_title + "'"),
            0);
  EXPECT_EQ(a.count("SELECT COUNT(*) FROM composition WHERE source = 1 AND NOT EXISTS ("
                    "  SELECT 1 FROM arrangement ar JOIN track_arrangement ta ON ta.arrangement_id = ar.id "
                    "  WHERE ar.composition_id = composition.id)"),
            0);
}

TEST_F(ExtTest, EmptiedCompositionStaysWhenSomethingPointsAtIt) {
  Approvals a{*s};
  auto title = a.title_match(), mb = a.mb_link();
  ASSERT_EQ(a.accept(title), 200);
  auto by_title = a.composition_of(a.f1);
  // Someone picked a favourite version of it: keep the composition (now empty).
  ASSERT_EQ(s->call("PUT", "/ext/compositions/" + by_title + "/preferred", json{{"songId", a.f1}}, &s->user_key).status,
            200);
  ASSERT_EQ(a.accept(mb), 200);
  EXPECT_EQ(s->get("/ext/compositions/" + by_title).status, 200);
}

TEST_F(ExtTest, GraphKeepsOnlyTheOriginsAskedFor) {
  Approvals a{*s};
  ASSERT_EQ(a.accept(a.title_match()), 200);
  const auto seed = "/ext/graph?seed=song:" + a.f1 + "&include=composition";
  auto performances = [&](const std::string& extra) {
    auto r = s->get(seed + extra);
    EXPECT_EQ(r.status, 200) << r.body.dump();
    std::vector<json> out;
    for (const auto& e : r.body["edges"])
      if (e["type"] == "performance") out.push_back(e);
    return out;
  };
  // Unfiltered, the estimated edge says which rule made it and what it rests on: the
  // built-in rules match names.
  auto all = performances("");
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0]["rule"], "same_composition");
  EXPECT_EQ(all[0]["origin"], "name");
  EXPECT_FALSE(all[0].contains("originRef"));
  // Only what an outside database states: the title match goes, and with it the other song.
  EXPECT_TRUE(performances("&origin=external,tag,manual").empty());
  EXPECT_TRUE(performances("&origin=").empty());  // empty: nothing but computed links
  EXPECT_EQ(s->get(seed + "&origin=guess").status, 400);
  ASSERT_EQ(a.accept(a.mb_link()), 200);
  auto mb = performances("&origin=external,tag,manual");
  ASSERT_EQ(mb.size(), 2u);
  EXPECT_EQ(mb[0]["rule"], "mb_match");
  EXPECT_EQ(mb[0]["origin"], "external");
  EXPECT_EQ(mb[0]["originRef"], "https://musicbrainz.org/work/mbid-field");
}

TEST_F(ExtTest, SuggestionsKeepTheirOrigin) {
  auto f1 = s->track("Other/01.flac"), f2 = s->track("Other/02.flac");
  auto post = [&](json extra) {
    json one = {{"rule", "tool"}, {"score", 0.5},
                {"proposal", {{"action", "link_title"}, {"titleName", "Field Game"}, {"tracks", {f1, f2}}}}};
    one.update(extra);
    return s->call("POST", "/ext/suggestions", json{{"suggestions", json::array({one})}});
  };
  // A word outside the list is refused for that item.
  auto bad = post({{"origin", "tag"}});
  EXPECT_TRUE(bad.body["created"].empty()) << bad.body.dump();
  EXPECT_EQ(bad.body["errors"].size(), 1u);
  // No origin: an LLM's guess, as before.
  auto r = post(json::object());
  ASSERT_EQ(r.status, 201) << r.body.dump();
  auto id = r.body["created"][0].get<std::string>();
  auto got = s->get("/ext/suggestions/" + id).body;
  EXPECT_EQ(got["origin"], "llm");
  EXPECT_TRUE(got["originRef"].is_null());

  // Accepting copies it onto the rows, and an export carries it to the next database.
  ASSERT_EQ(s->call("POST", "/ext/suggestions/" + id + "/accept").status, 200);
  EXPECT_EQ(s->conn->scalar_text("SELECT origin FROM track_title WHERE track_id = ?", f1), "llm");
  auto dump = s->get("/ext/export").body;
  ASSERT_EQ(dump["trackTitles"].size(), 2u);
  EXPECT_EQ(dump["trackTitles"][0]["origin"], "llm");
  s->conn->exec("UPDATE track_title SET origin = NULL");
  ASSERT_EQ(s->call("POST", "/ext/import", dump).status, 200);
  EXPECT_EQ(s->conn->scalar_text("SELECT origin FROM track_title WHERE track_id = ?", f1), "llm");
}

TEST_F(ExtTest, AcceptingATitleMatchKeepsNamesAsWritten) {
  Approvals a{*s};
  // The rule groups by normalized names ("kato"); the composer keeps the tag's spelling.
  auto before = s->conn->scalar_text("SELECT name FROM artist WHERE name_norm = 'kato'");
  ASSERT_TRUE(before.has_value());
  EXPECT_NE(*before, "kato");
  ASSERT_EQ(a.accept(a.title_match()), 200);
  EXPECT_EQ(s->conn->scalar_text("SELECT name FROM artist WHERE name_norm = 'kato'"), before);
}

TEST_F(ExtTest, VirtualAlbumCoverAndArtists) {
  auto f1 = s->track("Other/01.flac"), f2 = s->track("Other/02.flac");
  auto va = s->call("POST", "/ext/albums", json{{"name", "Field Game"}, {"tracks", {f1, f2}}});
  ASSERT_EQ(va.status, 201);
  const std::string path = "/ext/albums/" + va.body["id"].get<std::string>();
  const std::string id = va.body["id"];

  // A cover must be an image inside the library.
  fs::create_directories(music / "covers");
  std::ofstream(music / "covers/field.png", std::ios::binary) << test::make_png(8, 8);
  std::ofstream(music / "covers/notes.txt") << "not an image";
  EXPECT_EQ(s->call("PATCH", path, json{{"cover", "covers/missing.png"}}).status, 400);
  EXPECT_EQ(s->call("PATCH", path, json{{"cover", "covers/notes.txt"}}).status, 400);
  EXPECT_EQ(s->call("PATCH", path, json{{"cover", "../outside.png"}}).status, 400);
  ASSERT_EQ(s->call("PATCH", path, json{{"cover", "covers/field.png"}}).status, 200);
  EXPECT_EQ(s->conn->scalar_text(
                "SELECT value FROM field_override WHERE entity_type = 'album' AND entity_id = ? AND field = 'cover'", id),
            "covers/field.png");
  ASSERT_EQ(s->call("PATCH", path, json{{"cover", nullptr}}).status, 200);
  EXPECT_EQ(s->conn->scalar_int("SELECT COUNT(*) FROM field_override WHERE entity_id = ?", id), 0);

  // Album artists, the most important first; physical albums keep what their tags say.
  ASSERT_EQ(s->call("PATCH", path, json{{"albumArtists", {"Kato", "Sato"}}}).status, 200);
  EXPECT_EQ(s->conn->scalar_text("SELECT display_artist FROM album WHERE id = ?", id), "Kato / Sato");
  EXPECT_EQ(s->conn->scalar_int(
                "SELECT COUNT(*) FROM participant WHERE entity_type = 'album' AND entity_id = ? AND role = 'albumartist'", id),
            2);
  auto physical = s->conn->scalar_text("SELECT album_id FROM track WHERE id = ?", f1);
  EXPECT_EQ(s->call("PATCH", "/ext/albums/" + *physical, json{{"albumArtists", {"Kato"}}}).status, 409);
}
