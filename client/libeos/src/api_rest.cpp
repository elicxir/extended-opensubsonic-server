// /rest (OpenSubsonic) calls. See docs/rest-api.md.
#include "impl.h"
#include "parse.h"

namespace eos::client {
namespace {

using parse::object;

void add_ids(Query& q, const char* key, const std::vector<std::string>& ids) {
  for (const auto& id : ids) q.emplace_back(key, id);
}

void add_opt(Query& q, const char* key, const std::optional<int>& v) {
  if (v) q.emplace_back(key, std::to_string(*v));
}

}  // namespace

// ---- system ----

void Api::ping() { impl_->rest("ping"); }

User Api::user() {
  auto r = impl_->rest("getUser");
  return parsed("getUser", [&] {
    const json& u = object(r, "user");
    return User{parse::get<std::string>(u, "username"), parse::get<bool>(u, "adminRole")};
  });
}

ScanStatus Api::scan_status() {
  auto r = impl_->rest("getScanStatus");
  return parsed("getScanStatus", [&] { return parse::scan_status(object(r, "scanStatus")); });
}

ScanStatus Api::start_scan(bool full) {
  auto r = impl_->rest("startScan", {{"fullScan", full ? "true" : "false"}});
  return parsed("startScan", [&] { return parse::scan_status(object(r, "scanStatus")); });
}

// ---- browsing ----

Artists Api::artists() {
  auto r = impl_->rest("getArtists");
  return parsed("getArtists", [&] {
    const json& a = object(r, "artists");
    Artists out;
    out.ignored_articles = parse::get<std::string>(a, "ignoredArticles");
    out.index = parse::list(a, "index", [](const json& i) {
      return ArtistIndex{parse::get<std::string>(i, "name"), parse::list(i, "artist", parse::artist)};
    });
    return out;
  });
}

Artist Api::artist(const std::string& id) {
  auto r = impl_->rest("getArtist", {{"id", id}});
  return parsed("getArtist", [&] { return parse::artist(object(r, "artist")); });
}

Album Api::album(const std::string& id) {
  auto r = impl_->rest("getAlbum", {{"id", id}});
  return parsed("getAlbum", [&] { return parse::album(object(r, "album")); });
}

Song Api::song(const std::string& id) {
  auto r = impl_->rest("getSong", {{"id", id}});
  return parsed("getSong", [&] { return parse::song(object(r, "song")); });
}

std::vector<Genre> Api::genres() {
  auto r = impl_->rest("getGenres");
  return parsed("getGenres", [&] { return parse::list(object(r, "genres"), "genre", parse::genre); });
}

std::vector<Album> Api::album_list(const AlbumListQuery& q) {
  Query p{{"type", q.type}, {"size", std::to_string(q.size)}, {"offset", std::to_string(q.offset)}};
  add_opt(p, "fromYear", q.from_year);
  add_opt(p, "toYear", q.to_year);
  if (!q.genre.empty()) p.emplace_back("genre", q.genre);
  auto r = impl_->rest("getAlbumList2", std::move(p));
  return parsed("getAlbumList2", [&] { return parse::list(object(r, "albumList2"), "album", parse::album); });
}

std::vector<Song> Api::random_songs(const RandomSongsQuery& q) {
  Query p{{"size", std::to_string(q.size)}};
  if (!q.genre.empty()) p.emplace_back("genre", q.genre);
  add_opt(p, "fromYear", q.from_year);
  add_opt(p, "toYear", q.to_year);
  auto r = impl_->rest("getRandomSongs", std::move(p));
  return parsed("getRandomSongs", [&] { return parse::list(object(r, "randomSongs"), "song", parse::song); });
}

SearchResult Api::starred() {
  auto r = impl_->rest("getStarred2");
  return parsed("getStarred2", [&] { return parse::search_result(object(r, "starred2")); });
}

SearchResult Api::search(const SearchQuery& q) {
  auto r = impl_->rest("search3", {{"query", q.query},
                                   {"artistCount", std::to_string(q.artist_count)},
                                   {"artistOffset", std::to_string(q.artist_offset)},
                                   {"albumCount", std::to_string(q.album_count)},
                                   {"albumOffset", std::to_string(q.album_offset)},
                                   {"songCount", std::to_string(q.song_count)},
                                   {"songOffset", std::to_string(q.song_offset)}});
  return parsed("search3", [&] { return parse::search_result(object(r, "searchResult3")); });
}

// ---- stars, ratings, play counts ----

void Api::star(const std::vector<std::string>& ids) {
  if (ids.empty()) return;
  Query p;
  add_ids(p, "id", ids);
  impl_->rest("star", std::move(p));
}

void Api::unstar(const std::vector<std::string>& ids) {
  if (ids.empty()) return;
  Query p;
  add_ids(p, "id", ids);
  impl_->rest("unstar", std::move(p));
}

void Api::set_rating(const std::string& id, int rating) {
  impl_->rest("setRating", {{"id", id}, {"rating", std::to_string(rating)}});
}

void Api::scrobble(const std::string& song_id, std::optional<int64_t> time_ms) {
  Query p{{"id", song_id}, {"submission", "true"}};
  if (time_ms) p.emplace_back("time", std::to_string(*time_ms));
  impl_->rest("scrobble", std::move(p));
}

// ---- playlists ----

std::vector<Playlist> Api::playlists() {
  auto r = impl_->rest("getPlaylists");
  return parsed("getPlaylists", [&] { return parse::list(object(r, "playlists"), "playlist", parse::playlist); });
}

Playlist Api::playlist(const std::string& id) {
  auto r = impl_->rest("getPlaylist", {{"id", id}});
  return parsed("getPlaylist", [&] { return parse::playlist(object(r, "playlist")); });
}

Playlist Api::create_playlist(const std::string& name, const std::vector<std::string>& song_ids) {
  Query p{{"name", name}};
  add_ids(p, "songId", song_ids);
  auto r = impl_->rest("createPlaylist", std::move(p));
  return parsed("createPlaylist", [&] { return parse::playlist(object(r, "playlist")); });
}

void Api::update_playlist(const std::string& id, const PlaylistUpdate& u) {
  Query p{{"playlistId", id}};
  if (u.name) p.emplace_back("name", *u.name);
  if (u.comment) p.emplace_back("comment", *u.comment);
  if (u.is_public) p.emplace_back("public", *u.is_public ? "true" : "false");
  add_ids(p, "songIdToAdd", u.add_song_ids);
  for (int i : u.remove_indexes) p.emplace_back("songIndexToRemove", std::to_string(i));
  impl_->rest("updatePlaylist", std::move(p));
}

void Api::delete_playlist(const std::string& id) { impl_->rest("deletePlaylist", {{"id", id}}); }

// ---- play queue ----

PlayQueue Api::play_queue() {
  auto r = impl_->rest("getPlayQueueByIndex");
  return parsed("getPlayQueueByIndex", [&] { return parse::play_queue(object(r, "playQueueByIndex")); });
}

void Api::save_play_queue(const std::vector<std::string>& song_ids, int current_index, int64_t position_ms) {
  Query p;
  add_ids(p, "id", song_ids);
  if (!song_ids.empty()) {
    p.emplace_back("currentIndex", std::to_string(current_index));
    p.emplace_back("position", std::to_string(position_ms));
  }
  impl_->rest("savePlayQueueByIndex", std::move(p));
}

// ---- media ----

std::string Api::stream_url(const std::string& song_id, const StreamOptions& o) const {
  Query p{{"id", song_id}};
  if (!o.format.empty()) p.emplace_back("format", o.format);
  if (o.max_bit_rate > 0) p.emplace_back("maxBitRate", std::to_string(o.max_bit_rate));
  if (o.time_offset > 0) p.emplace_back("timeOffset", std::to_string(o.time_offset));
  return impl_->http.url("/rest/stream", impl_->media_query(std::move(p)));
}

std::string Api::download_url(const std::string& song_id) const {
  return impl_->http.url("/rest/download", impl_->media_query({{"id", song_id}}));
}

std::string Api::cover_art_url(const std::string& cover_art_id, int size) const {
  Query p{{"id", cover_art_id}};
  if (size > 0) p.emplace_back("size", std::to_string(size));
  return impl_->http.url("/rest/getCoverArt", impl_->media_query(std::move(p)));
}

Bytes Api::cover_art(const std::string& cover_art_id, int size) {
  Query p{{"id", cover_art_id}, {"f", "json"}};
  if (size > 0) p.emplace_back("size", std::to_string(size));
  auto res = impl_->http.send("GET", "/rest/getCoverArt", p);
  // Errors arrive as a Subsonic response (HTTP 200), the picture as image/*.
  if (res.content_type.starts_with("image/") && res.status == 200) return {std::move(res.body), res.content_type};
  auto doc = json::parse(res.body, nullptr, false);
  if (doc.is_object() && doc.contains("subsonic-response")) {
    const json& e = object(doc["subsonic-response"], "error");
    throw Error(Error::Kind::subsonic, parse::get<std::string>(e, "message", "Request failed"), res.status,
                parse::get<int>(e, "code", 0));
  }
  throw Error(Error::Kind::http, "HTTP " + std::to_string(res.status) + " from /rest/getCoverArt", res.status);
}

}  // namespace eos::client
