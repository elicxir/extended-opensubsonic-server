// What the server returns, as plain structs. Field names follow the JSON names in
// snake_case. Times are ISO 8601 (UTC) strings as the server sent them; durations are
// whole seconds. See docs/rest-api.md and docs/ext-api.md.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eos::client {

struct ArtistRef {
  std::string id, name;
};

struct Contributor {
  std::string role;      // composer, lyricist, arranger, conductor, performer
  std::string sub_role;  // the instrument of a performer, or empty
  ArtistRef artist;
};

struct ReplayGain {
  std::optional<double> track_gain, track_peak, album_gain, album_peak;
};

struct ItemDate {
  int year = 0;
  std::optional<int> month, day;
};

struct DiscTitle {
  int disc = 0;
  std::string title;
};

struct Song {
  std::string id, title;
  std::string album, album_id;  // always the physical album
  std::string artist, artist_id;
  std::string cover_art;  // pass to Api::cover_art() / cover_art_url()
  std::string genre, content_type, suffix, path;
  std::string created, sort_name, music_brainz_id;
  std::string display_artist, display_album_artist, display_composer;
  std::optional<int> track, disc_number, year;
  std::optional<int> bit_rate, bit_depth, sampling_rate, channel_count;
  int64_t size = 0;
  int duration = 0;
  int64_t play_count = 0;
  std::optional<std::string> played, starred;
  std::optional<int> user_rating;  // 1..5
  std::vector<std::string> genres;
  std::vector<ArtistRef> artists, album_artists;
  std::vector<Contributor> contributors;
  std::optional<ReplayGain> replay_gain;

  // Only from /ext (empty from /rest).
  std::optional<std::string> composition_id, arrangement_id;
  std::optional<std::string> arrangement_provenance;  // manual, tag, rule, llm
  std::optional<std::string> performance_kind;        // live, studio
  std::optional<std::string> recorded_date;
  std::vector<std::string> title_ids, virtual_album_ids;
};

struct Album {
  std::string id, name;
  std::string artist, artist_id;
  std::string cover_art, genre;
  std::string created, sort_name, music_brainz_id, display_artist;
  int song_count = 0;
  int duration = 0;
  int64_t play_count = 0;
  std::optional<std::string> played, starred;
  std::optional<int> user_rating;
  std::optional<int> year;
  bool is_compilation = false;
  std::vector<ArtistRef> artists;
  std::vector<std::string> genres;
  std::vector<DiscTitle> disc_titles;
  std::optional<ItemDate> release_date, original_release_date;
  std::vector<Song> songs;  // filled by Api::album() and Api::ext_album()

  // Only from /ext.
  std::string kind;        // physical, virtual
  std::string provenance;  // manual, tag, rule, llm
};

struct Artist {
  std::string id, name;
  int album_count = 0;
  std::optional<std::string> starred;
  std::optional<int> user_rating;
  std::string music_brainz_id, sort_name;
  std::vector<Album> albums;  // filled by Api::artist()
};

struct ArtistIndex {
  std::string name;  // "A", "カ", "#", ...
  std::vector<Artist> artists;
};

struct Artists {
  std::string ignored_articles;
  std::vector<ArtistIndex> index;
};

struct Genre {
  std::string name;
  int song_count = 0, album_count = 0;
};

// search3 and getStarred2.
struct SearchResult {
  std::vector<Artist> artists;
  std::vector<Album> albums;
  std::vector<Song> songs;
};

struct Playlist {
  std::string id, name, comment, owner;
  bool is_public = false;
  int song_count = 0;
  int duration = 0;
  std::string created, changed;
  std::vector<Song> songs;  // filled by Api::playlist()
};

struct PlayQueue {
  std::optional<std::string> current;  // song id (getPlayQueue)
  std::optional<int> current_index;    // (getPlayQueueByIndex)
  int64_t position_ms = 0;
  std::string username, changed, changed_by;
  std::vector<Song> songs;
};

struct ScanStatus {
  bool scanning = false;
  int64_t count = 0, folder_count = 0;
  std::optional<std::string> last_scan;
};

struct User {
  std::string username;
  bool admin = false;
};

// ---- /ext ----

template <class T>
struct Page {
  std::vector<T> items;
  int64_t total = 0;
};

struct Paging {
  int limit = 100;  // at most 500
  int offset = 0;
};

struct ServerInfo {
  int api_version = 0;
  std::string server_version;
  std::vector<std::string> features;
};

struct ExtScanStatus {
  struct Run {
    std::string kind, started_at;
    std::optional<std::string> finished_at;
    int64_t added = 0, updated = 0, moved = 0, missing = 0, errors = 0;
  };
  bool scanning = false;
  std::string phase;
  int64_t count = 0, folder_count = 0;
  std::optional<Run> last;
};

struct Folder {
  std::string id, name;
  std::optional<std::string> parent_id;
  std::optional<int> sort_order;
  int album_count = 0, child_count = 0;
  std::string provenance;
};

struct FolderDetail {
  Folder folder;
  std::vector<Folder> children;
  std::vector<Album> albums;
};

struct AlbumDetail {
  Album album;  // with songs
  std::vector<std::string> folder_ids;
  std::vector<Album> virtual_albums;  // physical album: the virtual albums holding its songs
  std::vector<Album> parent_albums;   // virtual album: the physical albums of its songs
};

// A title: the film, game or show a soundtrack belongs to.
struct Title {
  std::string id, name;
  std::optional<std::string> sort_name, kind;
  std::optional<int> year;
  std::string provenance;
  std::optional<int> song_count;  // in lists and search
};

struct TitleGroup {
  std::optional<Title> title;  // none: the songs without a title
  std::vector<Song> songs;
};

struct TitleDetail {
  Title title;
  std::vector<Album> albums;
  std::vector<Song> songs;
};

struct Composition {
  std::string id, name;
  std::optional<std::string> music_brainz_work_id;
  std::vector<ArtistRef> composers;
  std::string provenance;
};

struct Arrangement {
  std::string id, composition_id;
  std::optional<std::string> name;  // none for the original (implicit) arrangement
  std::string kind;
  bool is_implicit = false;
  std::vector<ArtistRef> arrangers;
  std::string provenance;
};

struct ArrangementSongs {
  Arrangement arrangement;
  std::vector<Song> songs;
};

struct RelationEnd {
  std::string type;  // track, composition
  std::string id;
};

struct Relation {
  std::string id;
  RelationEnd from, to;
  std::string type;  // medley, quote, derivative, cover, related
  std::optional<std::string> subtype, note;
  std::optional<int> position;
  std::string provenance;
  std::optional<double> confidence;  // estimated relations only
  std::string direction;             // out, in (as seen from the item asked about), or empty
};

struct RelatedItem {
  Relation relation;
  std::string target_type;  // track, composition
  std::optional<Song> song;
  std::optional<Composition> composition;
};

struct CompositionDetail {
  Composition composition;
  std::vector<ArrangementSongs> arrangements;
  std::vector<Relation> relations;
};

struct SongVersions {
  Song song;
  std::optional<Composition> composition;
  std::vector<ArrangementSongs> arrangements;  // other arrangements of the same composition
  std::vector<Song> performances;              // other songs of the same arrangement
  std::vector<RelatedItem> related;
};

struct ExtSearchResult {
  struct CompositionHit {
    Composition composition;
    std::vector<Song> songs;
  };
  struct TitleHit {
    Title title;
    int song_count = 0;
    std::vector<Song> songs;  // the first 10
  };
  std::vector<CompositionHit> compositions;
  std::vector<TitleHit> titles;
  std::vector<Song> songs;  // songs not already under a composition hit
  std::vector<Album> albums;
  std::vector<Artist> artists;
};

struct Preferred {
  std::string composition_id, song_id;
};

// GET /ext/graph: the neighbourhood of one item as nodes and edges. Groups (composition,
// title, artist, album) are nodes of their own; songs of one composition point at the
// composition node rather than at each other. No positions: the client lays it out.
struct GraphNode {
  std::string id;    // "<type>:<id>", e.g. "song:abc". Pass it as GraphQuery::seed to open the node
  std::string type;  // song, composition, title, artist, album
  std::string label;
  int distance = 0;  // steps from the seed; nodes come nearest first
  int degree = 0;    // neighbours of this node within the kinds asked for
  bool expanded = false;  // false: some neighbours are not in this graph; ask again with this node as seed
  // The item itself; the one matching `type` is set.
  std::optional<Song> song;
  std::optional<Composition> composition;
  std::optional<Title> title;
  std::optional<ArtistRef> artist;
  std::optional<Album> album;
};

struct GraphEdge {
  std::string from, to;  // node ids
  // performance (song -> composition), medley / quote / derivative / cover / related
  // (a relation), title (song -> title), credit (song or composition -> artist),
  // album (song -> album), similar (song -- song, with include=similar)
  std::string type;
  bool directed = false;  // relations only: from -> to means something
  std::string provenance;
  std::optional<double> confidence;  // estimated links only
  std::optional<std::string> rule;   // estimated links: the suggestion's rule, e.g. musicbrainz_work
  // What the link rests on: tag, manual, external, audio, name, llm; none when not known.
  std::optional<std::string> origin;
  std::optional<std::string> origin_ref;  // estimated links: where to look it up, e.g. a MusicBrainz URL
  // performance
  std::optional<std::string> arrangement_id, arrangement_name, arrangement_kind;
  bool is_original = false;
  // relations
  std::optional<std::string> relation_id, subtype, note;
  std::optional<int> position;
  // credit
  std::optional<std::string> role, sub_role;
  // album
  bool is_virtual = false;
  // similar: the kind of similarity (audio_embed, ...) and its score, whose scale depends on the kind
  std::optional<std::string> kind;
  std::optional<double> score;
};

struct Graph {
  std::string seed;  // node id of the centre
  int depth = 0;
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;  // every edge between two nodes in `nodes`
  bool truncated = false;        // the node limit was reached
};

struct ApiKeyInfo {
  std::string id, device_name, created_at;
  std::optional<std::string> last_used_at;
  bool revoked = false;
};

struct NewApiKey {
  std::string id, key, device_name;
};

struct LoginResult {
  std::string id, key, username;
  bool admin = false;
};

// What Api::token_login() hands back: put the three into Options to stay logged in.
struct TokenLogin {
  std::string username, token, salt;
};

struct Bytes {
  std::string data;
  std::string content_type;
};

}  // namespace eos::client
