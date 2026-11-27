#include "api/subsonic/builders.h"

#include <format>
#include <map>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "core/strings.h"

namespace eos::api {
namespace {

struct Person {
  std::string id, name, sub_role;
  int64_t source = 0;
};

// entity id -> role -> people, keeping only the highest source per (entity, role).
using PeopleMap = std::unordered_map<std::string, std::map<std::string, std::vector<Person>>>;

PeopleMap load_people(db::Connection& db, const char* entity_type, const std::string& ids_json) {
  PeopleMap out;
  auto st = db.prepare(
      "SELECT p.entity_id, p.role, p.sub_role, p.source, a.id, a.name FROM participant p "
      "JOIN artist a ON a.id = p.artist_id "
      "WHERE p.entity_type = ? AND p.entity_id IN (SELECT value FROM json_each(?)) "
      "ORDER BY p.entity_id, p.role, p.source DESC, p.position");
  st.bind_all(entity_type, ids_json);
  while (st.step()) {
    auto& people = out[st.text(0)][st.text(1)];
    auto source = st.int64(3);
    if (!people.empty() && people.front().source != source) continue;  // lower-priority source
    people.push_back({st.text(4), st.text(5), st.text(2), source});
  }
  return out;
}

std::unordered_map<std::string, std::vector<std::string>> load_track_genres(db::Connection& db,
                                                                            const std::string& ids_json) {
  std::unordered_map<std::string, std::vector<std::string>> out;
  auto st = db.prepare(
      "SELECT tg.track_id, g.name FROM track_genre tg JOIN genre g ON g.id = tg.genre_id "
      "WHERE tg.track_id IN (SELECT value FROM json_each(?)) ORDER BY tg.track_id, tg.position");
  st.bind(1, ids_json);
  while (st.step()) out[st.text(0)].push_back(st.text(1));
  return out;
}

std::vector<Node> id_name_list(const std::vector<Person>& people, const char* elem) {
  std::vector<Node> out;
  for (const auto& p : people) out.push_back(Node(elem).set("id", p.id).set("name", p.name));
  return out;
}

const std::vector<Person>& role(const PeopleMap& m, const std::string& id, const char* r) {
  static const std::vector<Person> none;
  auto it = m.find(id);
  if (it == m.end()) return none;
  auto jt = it->second.find(r);
  return jt == it->second.end() ? none : jt->second;
}

void put_annotation(Node& n, db::Statement& st, int col_play, int col_played, int col_starred,
                    int col_rating) {
  n.set("playCount", st.is_null(col_play) ? 0 : st.int64(col_play));
  if (!st.is_null(col_played)) n.set("played", iso_time(st.int64(col_played)));
  if (!st.is_null(col_starred)) n.set("starred", iso_time(st.int64(col_starred)));
  if (!st.is_null(col_rating) && st.int64(col_rating) > 0) n.set("userRating", st.int64(col_rating));
}

std::optional<Node> item_date(const char* elem, const std::optional<std::string>& date) {
  if (!date || date->size() < 4) return std::nullopt;
  auto y = parse_leading_int(date->substr(0, 4));
  if (!y) return std::nullopt;
  Node n(elem);
  n.set("year", *y);
  if (date->size() >= 7)
    if (auto m = parse_leading_int(date->substr(5, 2))) n.set("month", *m);
  if (date->size() >= 10)
    if (auto d = parse_leading_int(date->substr(8, 2))) n.set("day", *d);
  return n;
}

}  // namespace

std::string json_ids(const std::vector<std::string>& ids) { return nlohmann::json(ids).dump(); }

std::string visible_album_sql(bool show_virtual, const std::string& a) {
  return show_virtual ? a + ".missing = 0" : a + ".missing = 0 AND " + a + ".kind = 'physical'";
}

std::string album_cover_id(const std::string& album_id, int64_t updated_ms) {
  return std::format("al-{}_{}", album_id, updated_ms / 1000);
}
std::string track_cover_id(const std::string& track_id, int64_t updated_ms) {
  return std::format("mf-{}_{}", track_id, updated_ms / 1000);
}

std::vector<Node> Loader::songs(const std::vector<std::string>& ids, const std::string& elem) {
  std::vector<Node> out;
  if (ids.empty()) return out;
  const auto js = json_ids(ids);
  auto people = load_people(db_, "track", js);
  auto genres = load_track_genres(db_, js);
  std::vector<std::string> album_ids;
  {
    auto st = db_.prepare(
        "SELECT DISTINCT t.album_id FROM track t WHERE t.id IN (SELECT value FROM json_each(?))");
    st.bind(1, js);
    while (st.step()) album_ids.push_back(st.text(0));
  }
  auto album_people = load_people(db_, "album", json_ids(album_ids));

  auto st = db_.prepare(
      "SELECT t.id, t.title, t.album_id, al.name, t.display_artist, t.track_number, t.disc_number, "
      "t.year, t.size, t.content_type, t.suffix, t.duration_ms, t.bit_rate, t.path, t.created_at, "
      "t.updated_at, t.bit_depth, t.sample_rate, t.channels, t.mbz_recording_id, t.display_composer, "
      "t.sort_title, t.has_embedded_art, al.updated_at, al.display_artist, "
      "an.play_count, an.last_played_at, an.starred_at, an.rating "
      "FROM json_each(?1) j JOIN track t ON t.id = j.value JOIN album al ON al.id = t.album_id "
      "LEFT JOIN annotation an ON an.user_id = ?2 AND an.item_type = 'track' AND an.item_id = t.id "
      "WHERE t.missing = 0 ORDER BY j.key");
  st.bind_all(js, user_id_);
  while (st.step()) {
    const auto id = st.text(0);
    const auto album_id = st.text(2);
    Node n(elem);
    n.set("id", id).set("parent", album_id).set("isDir", false).set("title", st.text(1));
    n.set("album", st.text(3)).set("artist", st.text(4));
    if (!st.is_null(5)) n.set("track", st.int64(5));
    if (!st.is_null(7)) n.set("year", st.int64(7));
    const auto& g = genres[id];
    if (!g.empty()) n.set("genre", g.front());
    n.set("coverArt", st.int64(22) ? track_cover_id(id, st.int64(15))
                                   : album_cover_id(album_id, st.int64(23)));
    n.set("size", st.int64(8)).set("contentType", st.text(9)).set("suffix", st.text(10));
    n.set("duration", (st.int64(11) + 500) / 1000);
    if (!st.is_null(12)) n.set("bitRate", st.int64(12));
    n.set("path", st.text(13));
    put_annotation(n, st, 25, 26, 27, 28);
    if (!st.is_null(6)) n.set("discNumber", st.int64(6));
    n.set("created", iso_time(st.int64(14)));
    n.set("albumId", album_id);
    const auto& artists = role(people, id, "artist");
    if (!artists.empty()) n.set("artistId", artists.front().id);
    n.set("type", "music").set("mediaType", "song").set("isVideo", false);
    if (!st.is_null(16)) n.set("bitDepth", st.int64(16));
    if (!st.is_null(17)) n.set("samplingRate", st.int64(17));
    if (!st.is_null(18)) n.set("channelCount", st.int64(18));
    n.set("musicBrainzId", st.opt_text(19).value_or(""));
    n.set("sortName", st.opt_text(21).value_or(""));
    n.set("displayArtist", st.text(4));
    n.set("displayAlbumArtist", st.opt_text(24).value_or(""));
    n.set("displayComposer", st.opt_text(20).value_or(""));

    std::vector<Node> genre_nodes;
    for (const auto& name : g) genre_nodes.push_back(Node("genres").set("name", name));
    n.list("genres", std::move(genre_nodes), true);
    n.list("artists", id_name_list(artists, "artists"), true);
    n.list("albumArtists", id_name_list(role(album_people, album_id, "albumartist"), "albumArtists"),
           true);
    std::vector<Node> contributors;
    for (const char* r : {"composer", "lyricist", "arranger", "conductor", "performer"}) {
      for (const auto& p : role(people, id, r)) {
        Node c("contributors");
        c.set("role", r);
        if (!p.sub_role.empty()) c.set("subRole", p.sub_role);
        c.child(Node("artist").set("id", p.id).set("name", p.name));
        contributors.push_back(std::move(c));
      }
    }
    n.list("contributors", std::move(contributors), true);
    out.push_back(std::move(n));
  }
  return out;
}

std::vector<Node> Loader::albums(const std::vector<std::string>& ids, const std::string& elem) {
  std::vector<Node> out;
  if (ids.empty()) return out;
  const auto js = json_ids(ids);
  auto people = load_people(db_, "album", js);
  std::unordered_map<std::string, std::vector<std::pair<int64_t, std::string>>> discs;
  {
    auto st = db_.prepare(
        "SELECT album_id, disc_number, title FROM album_disc "
        "WHERE album_id IN (SELECT value FROM json_each(?)) ORDER BY album_id, disc_number");
    st.bind(1, js);
    while (st.step()) discs[st.text(0)].emplace_back(st.int64(1), st.text(2));
  }
  std::unordered_map<std::string, std::vector<std::string>> genres;
  {
    auto st = db_.prepare(
        "SELECT t.album_id, g.name, COUNT(*) AS n FROM track t "
        "JOIN track_genre tg ON tg.track_id = t.id JOIN genre g ON g.id = tg.genre_id "
        "WHERE t.missing = 0 AND t.album_id IN (SELECT value FROM json_each(?)) "
        "GROUP BY t.album_id, g.id ORDER BY t.album_id, n DESC, g.name");
    st.bind(1, js);
    while (st.step()) genres[st.text(0)].push_back(st.text(1));
  }
  auto st = db_.prepare(
      "SELECT al.id, al.name, al.display_artist, al.song_count, al.duration_ms, al.created_at, "
      "al.updated_at, al.year, al.mbz_album_id, al.sort_name, al.is_compilation, al.release_date, "
      "al.original_release_date, an.play_count, an.last_played_at, an.starred_at, an.rating "
      "FROM json_each(?1) j JOIN album al ON al.id = j.value "
      "LEFT JOIN annotation an ON an.user_id = ?2 AND an.item_type = 'album' AND an.item_id = al.id "
      "WHERE al.missing = 0 ORDER BY j.key");
  st.bind_all(js, user_id_);
  while (st.step()) {
    const auto id = st.text(0);
    const auto& artists = role(people, id, "albumartist");
    const auto& g = genres[id];
    Node n(elem);
    n.set("id", id).set("name", st.text(1)).set("artist", st.opt_text(2).value_or(""));
    if (!artists.empty()) n.set("artistId", artists.front().id);
    n.set("coverArt", album_cover_id(id, st.int64(6)));
    n.set("songCount", st.int64(3)).set("duration", (st.int64(4) + 500) / 1000);
    put_annotation(n, st, 13, 14, 15, 16);
    n.set("created", iso_time(st.int64(5)));
    if (!st.is_null(7)) n.set("year", st.int64(7));
    if (!g.empty()) n.set("genre", g.front());
    n.set("musicBrainzId", st.opt_text(8).value_or(""));
    n.set("sortName", st.opt_text(9).value_or(""));
    n.set("isCompilation", st.int64(10) != 0);
    n.set("displayArtist", st.opt_text(2).value_or(""));
    n.list("artists", id_name_list(artists, "artists"), true);
    std::vector<Node> genre_nodes;
    for (const auto& name : g) genre_nodes.push_back(Node("genres").set("name", name));
    n.list("genres", std::move(genre_nodes), true);
    std::vector<Node> disc_nodes;
    for (const auto& [disc, title] : discs[id])
      disc_nodes.push_back(Node("discTitles").set("disc", disc).set("title", title));
    n.list("discTitles", std::move(disc_nodes), true);
    if (auto d = item_date("releaseDate", st.opt_text(11))) n.child(std::move(*d));
    if (auto d = item_date("originalReleaseDate", st.opt_text(12))) n.child(std::move(*d));
    out.push_back(std::move(n));
  }
  return out;
}

std::vector<Node> Loader::artists(const std::vector<std::string>& ids, const std::string& elem) {
  std::vector<Node> out;
  if (ids.empty()) return out;
  auto st = db_.prepare(
      "SELECT ar.id, ar.name, ar.sort_name, ar.mbz_artist_id, "
      "  (SELECT COUNT(DISTINCT p.entity_id) FROM participant p JOIN album al ON al.id = p.entity_id "
      "   WHERE p.entity_type = 'album' AND p.role = 'albumartist' AND p.artist_id = ar.id "
      "     AND " + visible_album_sql(show_virtual_) + "), "
      "  an.starred_at, an.rating "
      "FROM json_each(?1) j JOIN artist ar ON ar.id = j.value "
      "LEFT JOIN annotation an ON an.user_id = ?2 AND an.item_type = 'artist' AND an.item_id = ar.id "
      "ORDER BY j.key");
  st.bind_all(json_ids(ids), user_id_);
  while (st.step()) {
    Node n(elem);
    n.set("id", st.text(0)).set("name", st.text(1)).set("albumCount", st.int64(4));
    if (!st.is_null(5)) n.set("starred", iso_time(st.int64(5)));
    if (!st.is_null(6) && st.int64(6) > 0) n.set("userRating", st.int64(6));
    n.set("musicBrainzId", st.opt_text(3).value_or(""));
    n.set("sortName", st.opt_text(2).value_or(""));
    out.push_back(std::move(n));
  }
  return out;
}

std::optional<Node> Loader::song(const std::string& id, const std::string& elem) {
  auto v = songs({id}, elem);
  if (v.empty()) return std::nullopt;
  return std::move(v.front());
}
std::optional<Node> Loader::album(const std::string& id, const std::string& elem) {
  auto v = albums({id}, elem);
  if (v.empty()) return std::nullopt;
  return std::move(v.front());
}
std::optional<Node> Loader::artist(const std::string& id, const std::string& elem) {
  auto v = artists({id}, elem);
  if (v.empty()) return std::nullopt;
  return std::move(v.front());
}

std::vector<std::string> Loader::album_track_ids(const std::string& album_id) {
  if (db_.scalar_text("SELECT kind FROM album WHERE id = ?", album_id) == "virtual") {
    return ids(
        "SELECT t.id FROM album_track at JOIN track t ON t.id = at.track_id "
        "WHERE at.album_id = ?1 AND at.excluded = 0 AND t.missing = 0 AND NOT EXISTS ("
        "  SELECT 1 FROM album_track x WHERE x.album_id = at.album_id AND x.track_id = at.track_id "
        "  AND x.excluded = 1) "
        "GROUP BY t.id ORDER BY COALESCE(MAX(at.disc_number), t.disc_number, 0), "
        "COALESCE(MAX(at.track_number), t.track_number, 0), t.album_id, t.path",
        album_id);
  }
  return ids(
      "SELECT id FROM track WHERE album_id = ? AND missing = 0 "
      "ORDER BY COALESCE(disc_number, 0), COALESCE(track_number, 0), path",
      album_id);
}

std::vector<std::string> Loader::artist_album_ids(const std::string& artist_id) {
  return ids(
      "SELECT al.id FROM participant p JOIN album al ON al.id = p.entity_id "
      "WHERE p.entity_type = 'album' AND p.role = 'albumartist' AND p.artist_id = ? "
      "AND " + visible_album_sql(show_virtual_) + " "
      "GROUP BY al.id ORDER BY COALESCE(al.year, 0) DESC, al.name",
      artist_id);
}

}  // namespace eos::api
