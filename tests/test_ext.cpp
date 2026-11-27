// M4: the /ext API, end to end through ExtRouter (no network).
#include <gtest/gtest.h>

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
  box("05.flac", 480, {{"TITLE", {"Main Theme 〜 Battle"}}, {"TRACKNUMBER", {"5"}},
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
  EXPECT_EQ(titles_of(groups[1]["songs"]), (std::set<std::string>{"Battle", "Main Theme 〜 Battle"}));

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
  ASSERT_EQ(v["arrangements"].size(), 1u);
  EXPECT_EQ(v["arrangements"][0]["arrangement"]["name"], "Piano Ver.");
  EXPECT_EQ(v["arrangements"][0]["songs"][0]["title"], "Main Theme (Piano Ver.)");
  EXPECT_TRUE(v["performances"].empty());
  EXPECT_TRUE(v["related"].empty());
  EXPECT_EQ(s->get("/ext/songs/nope/versions").status, 404);
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
  EXPECT_EQ(titles_of(r["songs"]), (std::set<std::string>{"Main Theme (TV Size)", "Main Theme 〜 Battle"}));
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
