#include <gtest/gtest.h>

#include "parse.h"

using namespace eos::client;
using nlohmann::json;

TEST(Parse, SongFromRest) {
  auto j = json::parse(R"({
    "id": "s1", "parent": "a1", "isDir": false, "title": "Opening", "album": "Album A", "artist": "Alice",
    "track": 1, "year": 2020, "genre": "Soundtrack", "coverArt": "mf-s1_170", "size": 12345,
    "contentType": "audio/flac", "suffix": "flac", "duration": 61, "bitRate": 900, "path": "A/01.flac",
    "playCount": 3, "played": "2026-01-02T03:04:05.000Z", "starred": "2026-01-01T00:00:00.000Z",
    "userRating": 4, "discNumber": 1, "created": "2025-12-01T00:00:00.000Z", "albumId": "a1",
    "artistId": "ar1", "type": "music", "bitDepth": 16, "samplingRate": 44100, "channelCount": 2,
    "musicBrainzId": "", "sortName": "opening", "displayArtist": "Alice", "displayAlbumArtist": "Alice",
    "displayComposer": "Carol",
    "genres": [{"name": "Soundtrack"}, {"name": "Game"}],
    "artists": [{"id": "ar1", "name": "Alice"}], "albumArtists": [{"id": "ar1", "name": "Alice"}],
    "contributors": [
      {"role": "composer", "artist": {"id": "ar2", "name": "Carol"}},
      {"role": "performer", "subRole": "piano", "artist": {"id": "ar3", "name": "Dave"}}],
    "replayGain": {"trackGain": -6.5, "trackPeak": 0.98}
  })");
  Song s = parse::song(j);
  EXPECT_EQ(s.id, "s1");
  EXPECT_EQ(s.title, "Opening");
  EXPECT_EQ(s.album_id, "a1");
  EXPECT_EQ(s.cover_art, "mf-s1_170");
  EXPECT_EQ(s.track, 1);
  EXPECT_EQ(s.duration, 61);
  EXPECT_EQ(s.size, 12345);
  EXPECT_EQ(s.play_count, 3);
  EXPECT_EQ(s.user_rating, 4);
  ASSERT_TRUE(s.starred);
  EXPECT_EQ(s.genres, (std::vector<std::string>{"Soundtrack", "Game"}));
  ASSERT_EQ(s.contributors.size(), 2u);
  EXPECT_EQ(s.contributors[1].sub_role, "piano");
  EXPECT_EQ(s.contributors[1].artist.name, "Dave");
  ASSERT_TRUE(s.replay_gain);
  EXPECT_DOUBLE_EQ(*s.replay_gain->track_gain, -6.5);
  EXPECT_FALSE(s.replay_gain->album_gain);
  // /rest songs carry no /ext fields.
  EXPECT_FALSE(s.composition_id);
  EXPECT_TRUE(s.title_ids.empty());
}

TEST(Parse, SongExtFieldsAndNulls) {
  auto j = json::parse(R"({
    "id": "s1", "title": "T", "compositionId": "c1", "arrangementId": "ar1", "arrangementProvenance": "llm",
    "titleIds": ["t1", "t2"], "virtualAlbumIds": [], "performanceKind": null, "recordedDate": "2020-01-02"
  })");
  Song s = parse::song(j);
  EXPECT_EQ(s.composition_id, "c1");
  EXPECT_EQ(s.arrangement_provenance, "llm");
  EXPECT_EQ(s.title_ids.size(), 2u);
  EXPECT_FALSE(s.performance_kind);
  EXPECT_EQ(s.recorded_date, "2020-01-02");
  EXPECT_FALSE(s.track);
  EXPECT_FALSE(s.replay_gain);
}

TEST(Parse, AlbumWithSongsAndDates) {
  auto j = json::parse(R"({
    "id": "a1", "name": "Album A", "artist": "Alice", "songCount": 2, "duration": 120, "isCompilation": true,
    "discTitles": [{"disc": 1, "title": "Disc One"}], "releaseDate": {"year": 2020, "month": 4, "day": 1},
    "originalReleaseDate": {"year": 1999}, "kind": "virtual", "provenance": "manual",
    "song": [{"id": "s1", "title": "One"}, {"id": "s2", "title": "Two"}]
  })");
  Album a = parse::album(j);
  EXPECT_TRUE(a.is_compilation);
  ASSERT_EQ(a.disc_titles.size(), 1u);
  EXPECT_EQ(a.disc_titles[0].title, "Disc One");
  ASSERT_TRUE(a.release_date);
  EXPECT_EQ(a.release_date->month, 4);
  ASSERT_TRUE(a.original_release_date);
  EXPECT_FALSE(a.original_release_date->month);
  EXPECT_EQ(a.kind, "virtual");
  ASSERT_EQ(a.songs.size(), 2u);
  EXPECT_EQ(a.songs[1].title, "Two");
}

TEST(Parse, PlayQueueCurrentFromIndex) {
  auto j = json::parse(R"({"currentIndex": 1, "position": 4200, "username": "alice",
    "entry": [{"id": "s1"}, {"id": "s2"}]})");
  PlayQueue q = parse::play_queue(j);
  EXPECT_EQ(q.current_index, 1);
  EXPECT_EQ(q.current, "s2");
  EXPECT_EQ(q.position_ms, 4200);
  EXPECT_EQ(parse::play_queue(json::object()).songs.size(), 0u);
  EXPECT_FALSE(parse::play_queue(json::object()).current);
}

TEST(Parse, RelationAndTarget) {
  auto j = json::parse(R"({
    "relation": {"id": "r1", "from": {"type": "track", "id": "s1"}, "to": {"type": "composition", "id": "c1"},
                 "type": "derivative", "subtype": "tv_size", "position": null, "note": null,
                 "provenance": "rule", "confidence": 0.9, "direction": "out"},
    "target": {"type": "composition", "composition": {"id": "c1", "name": "Main Theme",
               "musicBrainzWorkId": null, "composers": [{"id": "p1", "name": "Carol"}], "provenance": "tag"}}
  })");
  RelatedItem r = parse::related_item(j);
  EXPECT_EQ(r.relation.from.id, "s1");
  EXPECT_EQ(r.relation.to.type, "composition");
  EXPECT_EQ(r.relation.subtype, "tv_size");
  EXPECT_FALSE(r.relation.position);
  EXPECT_DOUBLE_EQ(*r.relation.confidence, 0.9);
  EXPECT_EQ(r.relation.direction, "out");
  EXPECT_EQ(r.target_type, "composition");
  ASSERT_TRUE(r.composition);
  EXPECT_EQ(r.composition->composers[0].name, "Carol");
  EXPECT_FALSE(r.song);
}

TEST(Parse, Graph) {
  auto j = json::parse(R"({
    "seed": "song:t1", "depth": 2, "truncated": true,
    "nodes": [
      {"id": "song:t1", "type": "song", "label": "Main Theme", "distance": 0, "degree": 2, "expanded": true,
       "song": {"id": "t1", "title": "Main Theme", "compositionId": "c1"}},
      {"id": "composition:c1", "type": "composition", "label": "Main Theme", "distance": 1, "degree": 3,
       "expanded": true, "composition": {"id": "c1", "name": "Main Theme", "musicBrainzWorkId": null,
       "composers": [{"id": "a1", "name": "Sato"}], "provenance": "tag"}},
      {"id": "title:w1", "type": "title", "label": "Game X", "distance": 1, "degree": 4, "expanded": false,
       "title": {"id": "w1", "name": "Game X", "sortName": null, "kind": "game", "year": 2021, "provenance": "tag"}},
      {"id": "artist:a1", "type": "artist", "label": "Sato", "distance": 1, "degree": 466, "expanded": false,
       "artist": {"id": "a1", "name": "Sato"}},
      {"id": "album:al1", "type": "album", "label": "Box", "distance": 1, "degree": 5, "expanded": false,
       "album": {"id": "al1", "name": "Box", "kind": "physical"}}
    ],
    "edges": [
      {"from": "song:t1", "to": "composition:c1", "type": "performance", "arrangementId": "ar1",
       "arrangementName": null, "arrangementKind": "original", "isOriginal": true, "provenance": "tag"},
      {"from": "song:t4", "to": "composition:c1", "type": "quote", "directed": true, "relationId": "r1",
       "subtype": null, "position": 2, "note": "intro", "provenance": "rule", "confidence": 0.75},
      {"from": "song:t1", "to": "title:w1", "type": "title", "provenance": "llm", "confidence": 0.6},
      {"from": "song:t1", "to": "artist:a1", "type": "credit", "role": "performer", "subRole": "piano",
       "provenance": "tag"},
      {"from": "song:t1", "to": "album:al1", "type": "album", "virtual": false, "provenance": "tag"}
    ]
  })");
  Graph g = parse::graph(j);
  EXPECT_EQ(g.seed, "song:t1");
  EXPECT_EQ(g.depth, 2);
  EXPECT_TRUE(g.truncated);
  ASSERT_EQ(g.nodes.size(), 5u);
  const auto& seed = g.nodes[0];
  EXPECT_EQ(seed.type, "song");
  EXPECT_EQ(seed.distance, 0);
  EXPECT_TRUE(seed.expanded);
  ASSERT_TRUE(seed.song);
  EXPECT_EQ(seed.song->composition_id, "c1");
  EXPECT_FALSE(seed.composition);
  ASSERT_TRUE(g.nodes[1].composition);
  EXPECT_EQ(g.nodes[1].composition->composers[0].name, "Sato");
  ASSERT_TRUE(g.nodes[2].title);
  EXPECT_EQ(g.nodes[2].title->year, 2021);
  EXPECT_FALSE(g.nodes[2].expanded);
  ASSERT_TRUE(g.nodes[3].artist);
  EXPECT_EQ(g.nodes[3].artist->name, "Sato");
  EXPECT_EQ(g.nodes[3].degree, 466);
  ASSERT_TRUE(g.nodes[4].album);
  EXPECT_EQ(g.nodes[4].album->name, "Box");

  ASSERT_EQ(g.edges.size(), 5u);
  const auto& perf = g.edges[0];
  EXPECT_EQ(perf.type, "performance");
  EXPECT_FALSE(perf.directed);
  EXPECT_EQ(perf.arrangement_id, "ar1");
  EXPECT_FALSE(perf.arrangement_name);
  EXPECT_EQ(perf.arrangement_kind, "original");
  EXPECT_TRUE(perf.is_original);
  EXPECT_FALSE(perf.confidence);
  const auto& quote = g.edges[1];
  EXPECT_TRUE(quote.directed);
  EXPECT_EQ(quote.relation_id, "r1");
  EXPECT_EQ(quote.position, 2);
  EXPECT_EQ(quote.note, "intro");
  EXPECT_DOUBLE_EQ(*quote.confidence, 0.75);
  EXPECT_EQ(g.edges[2].provenance, "llm");
  EXPECT_EQ(g.edges[3].role, "performer");
  EXPECT_EQ(g.edges[3].sub_role, "piano");
  EXPECT_FALSE(g.edges[4].is_virtual);

  // An empty answer and a node type the client does not know yet.
  auto other = parse::graph(json::parse(R"({"nodes": [{"id": "x:1", "type": "x", "label": "?"}]})"));
  ASSERT_EQ(other.nodes.size(), 1u);
  EXPECT_FALSE(other.nodes[0].song);
  EXPECT_TRUE(other.edges.empty());
}

TEST(Parse, WrongTypeThrows) {
  EXPECT_THROW(parse::song(json::parse(R"({"id": 5})")), json::exception);
}
