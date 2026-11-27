#include "parse.h"

namespace eos::client::parse {
namespace {

std::string name_of(const json& j) { return get<std::string>(j, "name"); }

ItemDate item_date(const json& j) {
  ItemDate d;
  d.year = get<int>(j, "year");
  d.month = opt<int>(j, "month");
  d.day = opt<int>(j, "day");
  return d;
}

}  // namespace

const json& object(const json& j, const char* key) {
  static const json empty = json::object();
  auto it = j.find(key);
  return it == j.end() || !it->is_object() ? empty : *it;
}

ArtistRef artist_ref(const json& j) { return {get<std::string>(j, "id"), get<std::string>(j, "name")}; }

Song song(const json& j) {
  Song s;
  s.id = get<std::string>(j, "id");
  s.title = get<std::string>(j, "title");
  s.album = get<std::string>(j, "album");
  s.album_id = get<std::string>(j, "albumId");
  s.artist = get<std::string>(j, "artist");
  s.artist_id = get<std::string>(j, "artistId");
  s.cover_art = get<std::string>(j, "coverArt");
  s.genre = get<std::string>(j, "genre");
  s.content_type = get<std::string>(j, "contentType");
  s.suffix = get<std::string>(j, "suffix");
  s.path = get<std::string>(j, "path");
  s.created = get<std::string>(j, "created");
  s.sort_name = get<std::string>(j, "sortName");
  s.music_brainz_id = get<std::string>(j, "musicBrainzId");
  s.display_artist = get<std::string>(j, "displayArtist");
  s.display_album_artist = get<std::string>(j, "displayAlbumArtist");
  s.display_composer = get<std::string>(j, "displayComposer");
  s.track = opt<int>(j, "track");
  s.disc_number = opt<int>(j, "discNumber");
  s.year = opt<int>(j, "year");
  s.bit_rate = opt<int>(j, "bitRate");
  s.bit_depth = opt<int>(j, "bitDepth");
  s.sampling_rate = opt<int>(j, "samplingRate");
  s.channel_count = opt<int>(j, "channelCount");
  s.size = get<int64_t>(j, "size");
  s.duration = get<int>(j, "duration");
  s.play_count = get<int64_t>(j, "playCount");
  s.played = opt<std::string>(j, "played");
  s.starred = opt<std::string>(j, "starred");
  s.user_rating = opt<int>(j, "userRating");
  s.genres = list(j, "genres", name_of);
  s.artists = list(j, "artists", artist_ref);
  s.album_artists = list(j, "albumArtists", artist_ref);
  s.contributors = list(j, "contributors", [](const json& c) {
    return Contributor{get<std::string>(c, "role"), get<std::string>(c, "subRole"), artist_ref(object(c, "artist"))};
  });
  s.replay_gain = maybe(j, "replayGain", [](const json& r) {
    return ReplayGain{opt<double>(r, "trackGain"), opt<double>(r, "trackPeak"), opt<double>(r, "albumGain"),
                      opt<double>(r, "albumPeak")};
  });
  s.composition_id = opt<std::string>(j, "compositionId");
  s.arrangement_id = opt<std::string>(j, "arrangementId");
  s.arrangement_provenance = opt<std::string>(j, "arrangementProvenance");
  s.performance_kind = opt<std::string>(j, "performanceKind");
  s.recorded_date = opt<std::string>(j, "recordedDate");
  s.title_ids = get<std::vector<std::string>>(j, "titleIds");
  s.virtual_album_ids = get<std::vector<std::string>>(j, "virtualAlbumIds");
  return s;
}

Album album(const json& j) {
  Album a;
  a.id = get<std::string>(j, "id");
  a.name = get<std::string>(j, "name");
  a.artist = get<std::string>(j, "artist");
  a.artist_id = get<std::string>(j, "artistId");
  a.cover_art = get<std::string>(j, "coverArt");
  a.genre = get<std::string>(j, "genre");
  a.created = get<std::string>(j, "created");
  a.sort_name = get<std::string>(j, "sortName");
  a.music_brainz_id = get<std::string>(j, "musicBrainzId");
  a.display_artist = get<std::string>(j, "displayArtist");
  a.song_count = get<int>(j, "songCount");
  a.duration = get<int>(j, "duration");
  a.play_count = get<int64_t>(j, "playCount");
  a.played = opt<std::string>(j, "played");
  a.starred = opt<std::string>(j, "starred");
  a.user_rating = opt<int>(j, "userRating");
  a.year = opt<int>(j, "year");
  a.is_compilation = get<bool>(j, "isCompilation");
  a.artists = list(j, "artists", artist_ref);
  a.genres = list(j, "genres", name_of);
  a.disc_titles = list(j, "discTitles", [](const json& d) {
    return DiscTitle{get<int>(d, "disc"), get<std::string>(d, "title")};
  });
  a.release_date = maybe(j, "releaseDate", item_date);
  a.original_release_date = maybe(j, "originalReleaseDate", item_date);
  a.songs = list(j, "song", song);
  a.kind = get<std::string>(j, "kind");
  a.provenance = get<std::string>(j, "provenance");
  return a;
}

Artist artist(const json& j) {
  Artist a;
  a.id = get<std::string>(j, "id");
  a.name = get<std::string>(j, "name");
  a.album_count = get<int>(j, "albumCount");
  a.starred = opt<std::string>(j, "starred");
  a.user_rating = opt<int>(j, "userRating");
  a.music_brainz_id = get<std::string>(j, "musicBrainzId");
  a.sort_name = get<std::string>(j, "sortName");
  a.albums = list(j, "album", album);
  return a;
}

Genre genre(const json& j) {
  return {get<std::string>(j, "value"), get<int>(j, "songCount"), get<int>(j, "albumCount")};
}

Playlist playlist(const json& j) {
  Playlist p;
  p.id = get<std::string>(j, "id");
  p.name = get<std::string>(j, "name");
  p.comment = get<std::string>(j, "comment");
  p.owner = get<std::string>(j, "owner");
  p.is_public = get<bool>(j, "public");
  p.song_count = get<int>(j, "songCount");
  p.duration = get<int>(j, "duration");
  p.created = get<std::string>(j, "created");
  p.changed = get<std::string>(j, "changed");
  p.songs = list(j, "entry", song);
  return p;
}

PlayQueue play_queue(const json& j) {
  PlayQueue q;
  q.current_index = opt<int>(j, "currentIndex");
  q.position_ms = get<int64_t>(j, "position");
  q.username = get<std::string>(j, "username");
  q.changed = get<std::string>(j, "changed");
  q.changed_by = get<std::string>(j, "changedBy");
  q.songs = list(j, "entry", song);
  if (auto id = opt<std::string>(j, "current")) {
    q.current = id;
  } else if (q.current_index && *q.current_index >= 0 && static_cast<size_t>(*q.current_index) < q.songs.size()) {
    q.current = q.songs[static_cast<size_t>(*q.current_index)].id;
  }
  return q;
}

ScanStatus scan_status(const json& j) {
  return {get<bool>(j, "scanning"), get<int64_t>(j, "count"), get<int64_t>(j, "folderCount"),
          opt<std::string>(j, "lastScan")};
}

SearchResult search_result(const json& j) {
  return {list(j, "artist", artist), list(j, "album", album), list(j, "song", song)};
}

Folder folder(const json& j) {
  Folder f;
  f.id = get<std::string>(j, "id");
  f.name = get<std::string>(j, "name");
  f.parent_id = opt<std::string>(j, "parentId");
  f.sort_order = opt<int>(j, "sortOrder");
  f.album_count = get<int>(j, "albumCount");
  f.child_count = get<int>(j, "childCount");
  f.provenance = get<std::string>(j, "provenance");
  return f;
}

Title title(const json& j) {
  Title t;
  t.id = get<std::string>(j, "id");
  t.name = get<std::string>(j, "name");
  t.sort_name = opt<std::string>(j, "sortName");
  t.kind = opt<std::string>(j, "kind");
  t.year = opt<int>(j, "year");
  t.provenance = get<std::string>(j, "provenance");
  t.song_count = opt<int>(j, "songCount");
  return t;
}

Composition composition(const json& j) {
  Composition c;
  c.id = get<std::string>(j, "id");
  c.name = get<std::string>(j, "name");
  c.music_brainz_work_id = opt<std::string>(j, "musicBrainzWorkId");
  c.composers = list(j, "composers", artist_ref);
  c.provenance = get<std::string>(j, "provenance");
  return c;
}

Arrangement arrangement(const json& j) {
  Arrangement a;
  a.id = get<std::string>(j, "id");
  a.composition_id = get<std::string>(j, "compositionId");
  a.name = opt<std::string>(j, "name");
  a.kind = get<std::string>(j, "kind");
  a.is_implicit = get<bool>(j, "isImplicit");
  a.arrangers = list(j, "arrangers", artist_ref);
  a.provenance = get<std::string>(j, "provenance");
  return a;
}

ArrangementSongs arrangement_songs(const json& j) {
  return {arrangement(object(j, "arrangement")), list(j, "songs", song)};
}

Relation relation(const json& j) {
  auto end = [](const json& e) { return RelationEnd{get<std::string>(e, "type"), get<std::string>(e, "id")}; };
  Relation r;
  r.id = get<std::string>(j, "id");
  r.from = end(object(j, "from"));
  r.to = end(object(j, "to"));
  r.type = get<std::string>(j, "type");
  r.subtype = opt<std::string>(j, "subtype");
  r.note = opt<std::string>(j, "note");
  r.position = opt<int>(j, "position");
  r.provenance = get<std::string>(j, "provenance");
  r.confidence = opt<double>(j, "confidence");
  r.direction = get<std::string>(j, "direction");
  return r;
}

RelatedItem related_item(const json& j) {
  RelatedItem r;
  r.relation = relation(object(j, "relation"));
  const json& target = object(j, "target");
  r.target_type = get<std::string>(target, "type");
  r.song = maybe(target, "song", song);
  r.composition = maybe(target, "composition", composition);
  return r;
}

Graph graph(const json& j) {
  Graph g;
  g.seed = get<std::string>(j, "seed");
  g.depth = get<int>(j, "depth");
  g.truncated = get<bool>(j, "truncated");
  g.nodes = list(j, "nodes", [](const json& n) {
    GraphNode node;
    node.id = get<std::string>(n, "id");
    node.type = get<std::string>(n, "type");
    node.label = get<std::string>(n, "label");
    node.distance = get<int>(n, "distance");
    node.degree = get<int>(n, "degree");
    node.expanded = get<bool>(n, "expanded");
    // The body sits under the key named after the type; other types keep only the fields above.
    if (node.type == "song") node.song = maybe(n, "song", song);
    else if (node.type == "composition") node.composition = maybe(n, "composition", composition);
    else if (node.type == "title") node.title = maybe(n, "title", title);
    else if (node.type == "artist") node.artist = maybe(n, "artist", artist_ref);
    else if (node.type == "album") node.album = maybe(n, "album", album);
    return node;
  });
  g.edges = list(j, "edges", [](const json& e) {
    GraphEdge edge;
    edge.from = get<std::string>(e, "from");
    edge.to = get<std::string>(e, "to");
    edge.type = get<std::string>(e, "type");
    edge.directed = get<bool>(e, "directed");
    edge.provenance = get<std::string>(e, "provenance");
    edge.confidence = opt<double>(e, "confidence");
    edge.arrangement_id = opt<std::string>(e, "arrangementId");
    edge.arrangement_name = opt<std::string>(e, "arrangementName");
    edge.arrangement_kind = opt<std::string>(e, "arrangementKind");
    edge.is_original = get<bool>(e, "isOriginal");
    edge.relation_id = opt<std::string>(e, "relationId");
    edge.subtype = opt<std::string>(e, "subtype");
    edge.note = opt<std::string>(e, "note");
    edge.position = opt<int>(e, "position");
    edge.role = opt<std::string>(e, "role");
    edge.sub_role = opt<std::string>(e, "subRole");
    edge.is_virtual = get<bool>(e, "virtual");
    edge.kind = opt<std::string>(e, "kind");
    edge.score = opt<double>(e, "score");
    return edge;
  });
  return g;
}

}  // namespace eos::client::parse
