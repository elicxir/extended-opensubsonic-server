// Api: blocking calls to /rest (OpenSubsonic) and /ext of an eos server.
//
//   eos::client::Api api({.base_url = "http://nas:4540", .api_key = key});
//   for (auto& album : api.album_list({.type = "newest", .size = 50})) ...
//
// - Every call blocks until the server answers, so call from a worker thread, not the
//   UI thread. One Api may be used from several threads at once; connections are kept
//   open and reused.
// - A failed call throws eos::client::Error (error.h).
// - ids are opaque strings. Cover art ids come from Song::cover_art / Album::cover_art.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "eos/client/error.h"
#include "eos/client/types.h"

namespace eos::client {

struct Options {
  std::string base_url;  // "http://host:4540", or with a path prefix behind a reverse proxy
  std::string api_key;   // per-device key (Api::login, eos-admin apikey create)
  std::string client_name = "libeos";  // the Subsonic `c` parameter and the User-Agent
  long connect_timeout_ms = 10'000;
  long timeout_ms = 30'000;  // per request
  // false: /ext reads leave out links that are only estimated (rule or LLM, not yet confirmed).
  bool include_estimated = true;
};

struct AlbumListQuery {
  // random, newest, highest, frequent, recent, starred, alphabeticalByName,
  // alphabeticalByArtist, byYear, byGenre
  std::string type = "newest";
  int size = 10;  // at most 500
  int offset = 0;
  std::optional<int> from_year, to_year;  // byYear
  std::string genre;                      // byGenre
};

struct RandomSongsQuery {
  int size = 10;
  std::string genre;
  std::optional<int> from_year, to_year;
};

struct SearchQuery {
  std::string query;  // empty: everything (then song_count may go up to 5000)
  int artist_count = 20, artist_offset = 0;
  int album_count = 20, album_offset = 0;
  int song_count = 20, song_offset = 0;
};

struct PlaylistUpdate {
  std::optional<std::string> name, comment;
  std::optional<bool> is_public;
  std::vector<std::string> add_song_ids;
  std::vector<int> remove_indexes;  // positions before the update
};

struct StreamOptions {
  std::string format;   // "", "raw", "opus", "mp3"
  int max_bit_rate = 0; // kbps; 0 = no limit
  int time_offset = 0;  // seconds; only when the server transcodes
};

struct GraphQuery {
  // The centre, as "<type>:<id>": song (or track), composition, title, artist, album.
  // GraphNode::id can be passed back as is.
  std::string seed;
  int depth = 2;  // 0..4; song -> composition -> other song is 2 steps
  // Kinds of edges to follow: composition, relation, title, artist, album.
  // Empty: the server default (composition, relation, title).
  std::vector<std::string> include;
  std::vector<std::string> roles;  // artist edges only, e.g. {"composer", "arranger"}; empty: all
  int limit = 200;   // most nodes (1..1000); the nearest are kept
  int fanout = 50;   // a node (other than the seed) with more neighbours is not opened
};

using Query = std::vector<std::pair<std::string, std::string>>;

class Api {
 public:
  explicit Api(Options options);
  ~Api();
  Api(Api&&) noexcept;
  Api& operator=(Api&&) noexcept;

  const Options& options() const;

  // Exchanges a username and password for a new API key for this device
  // (POST /ext/auth/login). `server.api_key` is not used.
  static LoginResult login(const Options& server, const std::string& username, const std::string& password,
                           const std::string& device_name);

  // ---- /rest: system ----
  void ping();  // throws unless the server answers and accepts the key
  User user();
  ScanStatus scan_status();
  ScanStatus start_scan(bool full = false);  // admin

  // ---- /rest: browsing ----
  Artists artists();
  Artist artist(const std::string& id);  // with albums
  Album album(const std::string& id);    // with songs
  Song song(const std::string& id);
  std::vector<Genre> genres();
  std::vector<Album> album_list(const AlbumListQuery& q = {});
  std::vector<Song> random_songs(const RandomSongsQuery& q = {});
  SearchResult starred();
  SearchResult search(const SearchQuery& q);

  // ---- /rest: stars, ratings, play counts ----
  // ids may be songs, albums or artists; the server tells them apart.
  void star(const std::vector<std::string>& ids);
  void unstar(const std::vector<std::string>& ids);
  void set_rating(const std::string& id, int rating);  // 1..5, 0 removes it
  // Counts one play. `time_ms` (UNIX ms) records a play that happened earlier.
  void scrobble(const std::string& song_id, std::optional<int64_t> time_ms = std::nullopt);

  // ---- /rest: playlists ----
  std::vector<Playlist> playlists();
  Playlist playlist(const std::string& id);  // with songs
  Playlist create_playlist(const std::string& name, const std::vector<std::string>& song_ids = {});
  void update_playlist(const std::string& id, const PlaylistUpdate& update);
  void delete_playlist(const std::string& id);

  // ---- /rest: play queue (the same queue as /ext/session) ----
  PlayQueue play_queue();
  // Replaces the whole queue. An empty `song_ids` clears it.
  void save_play_queue(const std::vector<std::string>& song_ids, int current_index = 0, int64_t position_ms = 0);

  // ---- /rest: media ----
  // URLs for a player (mpv). They carry the API key: hand them over IPC, keep them out
  // of command lines and logs.
  std::string stream_url(const std::string& song_id, const StreamOptions& o = {}) const;
  std::string download_url(const std::string& song_id) const;
  std::string cover_art_url(const std::string& cover_art_id, int size = 0) const;
  // Downloads cover art. `size` > 0 asks for a square of that many pixels.
  Bytes cover_art(const std::string& cover_art_id, int size = 0);

  // ---- /ext: reads ----
  ServerInfo info();
  ExtScanStatus ext_scan_status();
  std::vector<Folder> folders();  // flat; build the tree from parent_id
  FolderDetail folder(const std::string& id);
  // kind: all, physical, virtual
  Page<Album> ext_albums(const std::string& kind = "all", Paging paging = {});
  AlbumDetail ext_album(const std::string& id);
  std::vector<TitleGroup> album_titles(const std::string& album_id);
  Page<Title> titles(const std::string& query = {}, Paging paging = {});
  TitleDetail title(const std::string& id);
  Page<Composition> compositions(const std::string& query = {}, Paging paging = {});
  CompositionDetail composition(const std::string& id);
  Song ext_song(const std::string& id);
  SongVersions song_versions(const std::string& song_id);
  ExtSearchResult ext_search(const std::string& q, int limit = 20);  // per kind
  // Related songs as a graph around q.seed (GET /ext/graph). An unknown seed throws not_found.
  Graph graph(const GraphQuery& q);

  // ---- /ext: preferred version of a composition (per user) ----
  std::vector<Preferred> preferred();
  void set_preferred(const std::string& composition_id, const std::string& song_id);
  void clear_preferred(const std::string& composition_id);

  // ---- /ext: API keys of the logged-in user ----
  std::vector<ApiKeyInfo> api_keys();
  NewApiKey create_api_key(const std::string& device_name);
  void revoke_api_key(const std::string& id);

  // ---- /ext: anything not wrapped above (edits, suggestions, export) ----
  // `path` starts with "/ext/". `json_body` is sent as is when not empty. Returns the
  // response body (JSON text, empty for 204). Errors throw like every other call.
  std::string ext_request(const std::string& method, const std::string& path, const Query& query = {},
                          const std::string& json_body = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::client
