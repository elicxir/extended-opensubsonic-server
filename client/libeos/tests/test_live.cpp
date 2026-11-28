// Calls a real eosd (started by live.sh with a three-song library: Album A with
// "Opening" and "Battle", Album B with a piano version of the same composition).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>

#include "eos/client/api.h"

using namespace eos::client;

namespace {

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

class Live : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (env("EOS_TEST_URL").empty()) return;
    server.base_url = env("EOS_TEST_URL");
    server.client_name = "libeos-test";
    auto login = Api::login(server, env("EOS_TEST_USER"), env("EOS_TEST_PASSWORD"), "live-test");
    ASSERT_FALSE(login.key.empty());
    EXPECT_TRUE(login.admin);
    server.api_key = login.key;
  }
  void SetUp() override {
    if (env("EOS_TEST_URL").empty()) GTEST_SKIP() << "EOS_TEST_URL is not set";
    ASSERT_FALSE(server.api_key.empty()) << "login failed";
  }

  Api api{server};

  Song find_song(const std::string& title) {
    for (auto& s : api.search({.query = "", .song_count = 100}).songs)
      if (s.title == title) return s;
    ADD_FAILURE() << "no song " << title;
    return {};
  }

  static inline Options server;
};

TEST_F(Live, PingUserInfo) {
  api.ping();
  auto u = api.user();
  EXPECT_EQ(u.username, "alice");
  EXPECT_TRUE(u.admin);
  auto info = api.info();
  EXPECT_EQ(info.api_version, 1);
  EXPECT_FALSE(info.server_version.empty());
  EXPECT_FALSE(info.features.empty());
  EXPECT_FALSE(api.scan_status().scanning);
  auto scan = api.ext_scan_status();
  EXPECT_EQ(scan.phase, "idle");
  ASSERT_TRUE(scan.last);  // eosd scans once more when it starts, so nothing was added
  EXPECT_FALSE(scan.last->kind.empty());
  EXPECT_TRUE(scan.last->finished_at);
  EXPECT_EQ(scan.last->errors, 0);
}

TEST_F(Live, WrongKeyAndWrongPassword) {
  Options bad = server;
  bad.api_key = "nope";
  try {
    Api(bad).ping();
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.kind(), Error::Kind::subsonic);
    EXPECT_EQ(e.subsonic_code(), 44);
    EXPECT_TRUE(e.unauthorized());
  }
  try {
    Api(bad).info();
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.kind(), Error::Kind::ext);
    EXPECT_EQ(e.http_status(), 401);
    EXPECT_EQ(e.ext_code(), "unauthorized");
    EXPECT_TRUE(e.unauthorized());
  }
  try {
    Api::login(server, "alice", "wrong", "x");
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.unauthorized());
  }
}

TEST_F(Live, Browse) {
  auto artists = api.artists();
  std::vector<std::string> names;
  for (auto& i : artists.index)
    for (auto& a : i.artists) names.push_back(a.name);
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"Alice", "The Erin Band"}));

  auto albums = api.album_list({.type = "alphabeticalByName", .size = 50});
  ASSERT_EQ(albums.size(), 2u);
  EXPECT_EQ(albums[0].name, "Album A");
  EXPECT_EQ(albums[0].song_count, 2);
  EXPECT_EQ(albums[0].year, 2020);
  ASSERT_FALSE(albums[0].artists.empty());

  auto artist = api.artist(albums[0].artists[0].id);
  EXPECT_EQ(artist.name, "Alice");
  ASSERT_EQ(artist.albums.size(), 1u);

  auto album = api.album(albums[0].id);
  ASSERT_EQ(album.songs.size(), 2u);
  const Song& s = album.songs[0];
  EXPECT_EQ(s.title, "Opening");
  EXPECT_EQ(s.track, 1);
  EXPECT_EQ(s.album_id, album.id);
  EXPECT_EQ(s.suffix, "flac");
  EXPECT_EQ(s.duration, 2);
  EXPECT_EQ(s.display_composer, "Carol");
  ASSERT_TRUE(s.replay_gain);
  EXPECT_DOUBLE_EQ(*s.replay_gain->track_gain, -6.5);
  EXPECT_EQ(album.songs[1].artists.size(), 2u);

  EXPECT_EQ(api.song(s.id).title, "Opening");
  auto genres = api.genres();
  EXPECT_EQ(genres.size(), 2u);
  EXPECT_FALSE(genres[0].name.empty());
  EXPECT_EQ(api.random_songs({.size = 10}).size(), 3u);

  try {
    api.album("does-not-exist");
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.not_found());
    EXPECT_EQ(e.subsonic_code(), 70);
  }
}

TEST_F(Live, SearchWithJapaneseAndSymbols) {
  auto r = api.search({.query = "メインテーマ"});
  ASSERT_EQ(r.songs.size(), 1u);
  EXPECT_EQ(r.songs[0].title, "メインテーマ (Piano)");
  auto band = api.search({.query = "erin band"});  // every word must match
  ASSERT_EQ(band.albums.size(), 1u);
  EXPECT_EQ(band.albums[0].name, "Album B");
  EXPECT_EQ(band.artists.size(), 1u);
  EXPECT_EQ(api.search({.query = "", .song_count = 100}).songs.size(), 3u);
  // Characters that would break an unescaped form body.
  EXPECT_TRUE(api.search({.query = "a&b=c%+d"}).songs.empty());
}

TEST_F(Live, StarRateScrobble) {
  auto s = find_song("Battle");
  api.star({s.id, s.album_id});
  auto starred = api.starred();
  ASSERT_EQ(starred.songs.size(), 1u);
  EXPECT_EQ(starred.songs[0].id, s.id);
  EXPECT_EQ(starred.albums.size(), 1u);
  api.unstar({s.id, s.album_id});
  EXPECT_TRUE(api.starred().songs.empty());

  api.set_rating(s.id, 5);
  EXPECT_EQ(api.song(s.id).user_rating, 5);
  api.set_rating(s.id, 0);
  EXPECT_FALSE(api.song(s.id).user_rating);

  auto before = api.song(s.id).play_count;
  api.scrobble(s.id);
  api.scrobble(s.id, 1'700'000'000'000);
  auto after = api.song(s.id);
  EXPECT_EQ(after.play_count, before + 2);
  EXPECT_TRUE(after.played);
}

TEST_F(Live, Playlists) {
  auto a = find_song("Opening"), b = find_song("Battle");
  auto p = api.create_playlist("プレイリスト 1", {a.id, b.id});
  EXPECT_EQ(p.name, "プレイリスト 1");
  ASSERT_EQ(p.songs.size(), 2u);
  EXPECT_EQ(p.owner, "alice");

  api.update_playlist(p.id, {.name = "Renamed", .comment = "c", .is_public = true, .add_song_ids = {a.id},
                             .remove_indexes = {0}});
  auto got = api.playlist(p.id);
  EXPECT_EQ(got.name, "Renamed");
  EXPECT_EQ(got.comment, "c");
  EXPECT_TRUE(got.is_public);
  ASSERT_EQ(got.songs.size(), 2u);
  EXPECT_EQ(got.songs[0].id, b.id);
  EXPECT_EQ(got.songs[1].id, a.id);

  auto all = api.playlists();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].song_count, 2);
  api.delete_playlist(p.id);
  EXPECT_TRUE(api.playlists().empty());
  EXPECT_THROW(api.playlist(p.id), Error);
}

TEST_F(Live, PlayQueue) {
  auto a = find_song("Opening"), b = find_song("Battle");
  api.save_play_queue({a.id, b.id, a.id}, 1, 4200);
  auto q = api.play_queue();
  ASSERT_EQ(q.songs.size(), 3u);
  EXPECT_EQ(q.current_index, 1);
  EXPECT_EQ(q.current, b.id);
  EXPECT_EQ(q.position_ms, 4200);
  EXPECT_EQ(q.changed_by, "libeos-test");
  api.save_play_queue({});
  EXPECT_TRUE(api.play_queue().songs.empty());
}

TEST_F(Live, ManyIdsFitInOneRequest) {
  auto a = find_song("Opening");
  std::vector<std::string> ids(3000, a.id);
  api.save_play_queue(ids, 2999, 0);
  auto q = api.play_queue();
  EXPECT_EQ(q.songs.size(), 3000u);
  EXPECT_EQ(q.current_index, 2999);
  api.save_play_queue({});
}

TEST_F(Live, CoverArtAndMediaUrls) {
  auto s = find_song("Opening");
  auto art = api.cover_art(s.cover_art);
  EXPECT_TRUE(art.content_type.starts_with("image/"));
  EXPECT_GT(art.data.size(), 50u);
  auto small = api.cover_art(s.cover_art, 32);
  EXPECT_FALSE(small.data.empty());
  try {
    api.cover_art("al-missing_0");
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.not_found()) << e.what();
  }
  EXPECT_NE(api.stream_url(s.id).find("/rest/stream?id=" + s.id), std::string::npos);
}

TEST_F(Live, ExtFoldersAlbumsTitles) {
  auto folders = api.folders();
  ASSERT_EQ(folders.size(), 2u);
  const Folder* root = nullptr;
  const Folder* child = nullptr;
  for (auto& f : folders) (f.parent_id ? child : root) = &f;
  ASSERT_TRUE(root && child);
  EXPECT_EQ(root->name, "Soundtracks");
  EXPECT_EQ(child->name, "Series A");
  EXPECT_EQ(child->parent_id, root->id);
  EXPECT_EQ(root->child_count, 1);
  EXPECT_EQ(child->provenance, "tag");

  auto detail = api.folder(root->id);
  ASSERT_EQ(detail.children.size(), 1u);
  auto leaf = api.folder(child->id);
  ASSERT_EQ(leaf.albums.size(), 1u);
  EXPECT_EQ(leaf.albums[0].name, "Album A");
  EXPECT_EQ(leaf.albums[0].kind, "physical");

  auto all = api.ext_albums();
  EXPECT_EQ(all.total, 3);
  auto virt = api.ext_albums("virtual");
  ASSERT_EQ(virt.items.size(), 1u);
  EXPECT_EQ(virt.items[0].name, "Best Of");
  auto paged = api.ext_albums("all", {.limit = 1, .offset = 1});
  EXPECT_EQ(paged.items.size(), 1u);
  EXPECT_EQ(paged.total, 3);

  auto best = api.ext_album(virt.items[0].id);
  EXPECT_EQ(best.album.kind, "virtual");
  ASSERT_EQ(best.album.songs.size(), 1u);
  EXPECT_EQ(best.album.songs[0].title, "Opening");
  EXPECT_EQ(best.album.songs[0].virtual_album_ids, (std::vector<std::string>{best.album.id}));
  ASSERT_EQ(best.parent_albums.size(), 1u);
  EXPECT_EQ(best.parent_albums[0].name, "Album A");

  auto physical = api.ext_album(leaf.albums[0].id);
  EXPECT_EQ(physical.folder_ids, (std::vector<std::string>{child->id}));
  ASSERT_EQ(physical.virtual_albums.size(), 1u);

  auto titles = api.titles();
  ASSERT_EQ(titles.items.size(), 1u);
  EXPECT_EQ(titles.items[0].name, "Game Z");
  EXPECT_EQ(titles.items[0].song_count, 2);
  EXPECT_TRUE(api.titles("nothing").items.empty());
  auto title = api.title(titles.items[0].id);
  EXPECT_EQ(title.songs.size(), 2u);
  EXPECT_EQ(title.albums.size(), 1u);

  auto groups = api.album_titles(leaf.albums[0].id);
  ASSERT_EQ(groups.size(), 1u);
  ASSERT_TRUE(groups[0].title);
  EXPECT_EQ(groups[0].title->name, "Game Z");
  EXPECT_EQ(groups[0].songs.size(), 2u);
  auto no_title = api.album_titles(api.search({.query = "erin band"}).albums.at(0).id);
  ASSERT_EQ(no_title.size(), 1u);
  EXPECT_FALSE(no_title[0].title);

  try {
    api.folder("missing");
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.not_found());
    EXPECT_EQ(e.ext_code(), "not_found");
  }
}

TEST_F(Live, ExtCompositionsVersionsSearchPreferred) {
  auto comps = api.compositions("main");
  ASSERT_EQ(comps.items.size(), 1u);
  const auto& comp = comps.items[0];
  EXPECT_EQ(comp.name, "Main Theme");
  EXPECT_EQ(comp.provenance, "tag");

  auto detail = api.composition(comp.id);
  ASSERT_EQ(detail.arrangements.size(), 2u);
  EXPECT_TRUE(detail.arrangements[0].arrangement.is_implicit);
  EXPECT_FALSE(detail.arrangements[0].arrangement.name);
  EXPECT_EQ(detail.arrangements[1].arrangement.name, "Piano Ver.");
  EXPECT_EQ(detail.arrangements[1].arrangement.kind, "piano");
  ASSERT_EQ(detail.arrangements[1].songs.size(), 1u);

  auto opening = find_song("Opening");
  auto ext = api.ext_song(opening.id);
  EXPECT_EQ(ext.composition_id, comp.id);
  EXPECT_EQ(ext.arrangement_provenance, "tag");
  EXPECT_EQ(ext.title_ids.size(), 1u);

  auto versions = api.song_versions(opening.id);
  EXPECT_EQ(versions.song.id, opening.id);
  ASSERT_TRUE(versions.composition);
  EXPECT_EQ(versions.composition->id, comp.id);
  ASSERT_EQ(versions.arrangements.size(), 1u);
  EXPECT_EQ(versions.arrangements[0].songs[0].title, "メインテーマ (Piano)");
  EXPECT_TRUE(versions.performances.empty());

  auto song_detail = api.song_detail(opening.id);
  EXPECT_EQ(song_detail.song.id, opening.id);
  ASSERT_TRUE(song_detail.composition && song_detail.arrangement && song_detail.arrangement_link);
  EXPECT_EQ(song_detail.composition->id, comp.id);
  EXPECT_TRUE(song_detail.arrangement->is_implicit);
  EXPECT_EQ(song_detail.arrangement_link->provenance, "tag");
  EXPECT_EQ(song_detail.album.id, opening.album_id);
  EXPECT_EQ(song_detail.titles.size(), 1u);
  EXPECT_FALSE(song_detail.file.path.empty());
  EXPECT_GT(song_detail.file.size, 0);
  EXPECT_FALSE(song_detail.tags.empty());

  auto found = api.ext_search("main theme");
  ASSERT_EQ(found.compositions.size(), 1u);
  EXPECT_EQ(found.compositions[0].songs.size(), 2u);
  auto game = api.ext_search("Game Z");
  ASSERT_EQ(game.titles.size(), 1u);
  EXPECT_EQ(game.titles[0].song_count, 2);

  EXPECT_TRUE(api.preferred().empty());
  api.set_preferred(comp.id, opening.id);
  auto pref = api.preferred();
  ASSERT_EQ(pref.size(), 1u);
  EXPECT_EQ(pref[0].composition_id, comp.id);
  EXPECT_EQ(pref[0].song_id, opening.id);
  api.clear_preferred(comp.id);
  EXPECT_TRUE(api.preferred().empty());
}

TEST_F(Live, Graph) {
  auto opening = find_song("Opening");
  auto node = [](const Graph& g, const std::string& type, const std::string& label) -> const GraphNode* {
    for (const auto& n : g.nodes)
      if (n.type == type && n.label == label) return &n;
    return nullptr;
  };

  // Two steps: the composition and the title, then the songs they hold.
  auto g = api.graph({.seed = "song:" + opening.id});
  EXPECT_EQ(g.seed, "song:" + opening.id);
  EXPECT_EQ(g.depth, 2);
  EXPECT_FALSE(g.truncated);
  ASSERT_EQ(g.nodes.size(), 5u);  // Opening, Main Theme, Game Z, the piano version, Battle
  EXPECT_EQ(g.nodes[0].id, "song:" + opening.id);
  EXPECT_EQ(g.nodes[0].distance, 0);
  ASSERT_TRUE(g.nodes[0].song);
  EXPECT_EQ(g.nodes[0].song->title, "Opening");
  auto* comp = node(g, "composition", "Main Theme");
  ASSERT_NE(comp, nullptr);
  ASSERT_TRUE(comp->composition);
  EXPECT_EQ(comp->distance, 1);
  EXPECT_EQ(comp->degree, 2);
  EXPECT_TRUE(comp->expanded);
  auto* game = node(g, "title", "Game Z");
  ASSERT_NE(game, nullptr);
  ASSERT_TRUE(game->title);
  auto* piano = node(g, "song", "メインテーマ (Piano)");
  ASSERT_NE(piano, nullptr);
  EXPECT_EQ(piano->distance, 2);
  ASSERT_EQ(g.edges.size(), 4u);
  int performances = 0;
  for (const auto& e : g.edges) {
    if (e.type != "performance") continue;
    ++performances;
    EXPECT_EQ(e.to, comp->id);
    EXPECT_EQ(e.provenance, "tag");
    if (e.from == piano->id) {
      EXPECT_FALSE(e.is_original);
      EXPECT_EQ(e.arrangement_name, "Piano Ver.");
    } else {
      EXPECT_TRUE(e.is_original);
    }
  }
  EXPECT_EQ(performances, 2);

  // One step, composers only: the composer node, not opened.
  auto people = api.graph({.seed = g.nodes[0].id, .depth = 1, .include = {"artist"}, .roles = {"composer"}});
  auto* carol = node(people, "artist", "Carol");
  ASSERT_NE(carol, nullptr);
  ASSERT_TRUE(carol->artist);
  for (const auto& e : people.edges) {
    EXPECT_EQ(e.type, "credit");
    EXPECT_EQ(e.role, "composer");
  }

  // A small limit cuts the walk.
  EXPECT_TRUE(api.graph({.seed = "song:" + opening.id, .limit = 2}).truncated);

  try {
    api.graph({.seed = "song:missing"});
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.not_found());
  }
  try {
    api.graph({.seed = "nonsense"});
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.http_status(), 400);
  }
}

TEST_F(Live, GraphSimilar) {
  auto opening = find_song("Opening"), battle = find_song("Battle"), piano = find_song("メインテーマ (Piano)");
  const GraphQuery q{.seed = "song:" + opening.id, .include = {"similar"}};
  try {
    api.graph(q);  // nothing stored yet
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.http_status(), 400);
  }

  // Store a similarity through the raw door (what audio-embed does), then read it as edges.
  api.ext_request("PUT", "/ext/similarity/audio_embed", {},
                  R"({"model": "live-test", "pairs": [[")" + opening.id + R"(", ")" + battle.id + R"(", 0.9], [")" +
                      opening.id + R"(", ")" + piano.id + R"(", 0.8]]})");
  auto g = api.graph(q);
  ASSERT_EQ(g.nodes.size(), 2u);  // Battle is on the same album
  EXPECT_EQ(g.nodes[1].id, "song:" + piano.id);
  ASSERT_EQ(g.edges.size(), 1u);
  EXPECT_EQ(g.edges[0].type, "similar");
  EXPECT_EQ(g.edges[0].kind, "audio_embed");
  EXPECT_DOUBLE_EQ(*g.edges[0].score, 0.8);

  auto all = q;
  all.similar_other_albums = false;
  g = api.graph(all);
  ASSERT_EQ(g.nodes.size(), 3u);
  EXPECT_EQ(g.nodes[1].id, "song:" + battle.id);  // best first
  all.similar_min_score = 0.85;
  EXPECT_EQ(api.graph(all).nodes.size(), 2u);
  all.similar_min_score.reset();
  all.similar_k = 1;
  EXPECT_EQ(api.graph(all).nodes.size(), 2u);
  all.similar_kind = "audio_match";
  EXPECT_THROW(api.graph(all), Error);

  EXPECT_EQ(api.ext_request("DELETE", "/ext/similarity/audio_embed"), "");
}

TEST_F(Live, ApiKeysAndRawRequests) {
  auto created = api.create_api_key("second device");
  EXPECT_FALSE(created.key.empty());
  Options other = server;
  other.api_key = created.key;
  Api(other).ping();

  auto keys = api.api_keys();
  EXPECT_GE(keys.size(), 2u);
  api.revoke_api_key(created.id);
  EXPECT_THROW(Api(other).ping(), Error);
  for (auto& k : api.api_keys())
    if (k.id == created.id) EXPECT_TRUE(k.revoked);

  // An edit through the raw door: create a title, then delete it.
  auto body = api.ext_request("POST", "/ext/titles", {}, R"({"name": "Raw Title"})");
  EXPECT_NE(body.find("Raw Title"), std::string::npos);
  auto titles = api.titles("raw title");
  ASSERT_EQ(titles.items.size(), 1u);
  EXPECT_EQ(titles.items[0].provenance, "manual");
  EXPECT_EQ(api.ext_request("DELETE", "/ext/titles/" + titles.items[0].id), "");
  try {
    api.ext_request("POST", "/ext/titles", {}, "{not json");
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.http_status(), 400);
    EXPECT_EQ(e.ext_code(), "bad_request");
  }
}

TEST_F(Live, NonAdminIsForbidden) {
  auto login = Api::login(server, env("EOS_TEST_USER2"), env("EOS_TEST_PASSWORD2"), "bob-device");
  EXPECT_FALSE(login.admin);
  Options o = server;
  o.api_key = login.key;
  Api bob(o);
  EXPECT_FALSE(bob.user().admin);
  try {
    bob.start_scan();
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.forbidden());
  }
  try {
    bob.ext_request("POST", "/ext/titles", {}, R"({"name": "X"})");
    FAIL();
  } catch (const Error& e) {
    EXPECT_TRUE(e.forbidden());
    EXPECT_EQ(e.ext_code(), "forbidden");
  }
}

TEST_F(Live, ConcurrentCallsShareOneApi) {
  std::vector<std::thread> threads;
  std::atomic<int> ok{0};
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < 20; ++i)
        if (api.album_list({.size = 5}).size() == 2) ++ok;
    });
  for (auto& t : threads) t.join();
  EXPECT_EQ(ok, 160);
}

}  // namespace
