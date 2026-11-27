// search3 on the FTS5 trigram index (design: 検索). An empty query lists everything.
#include "api/subsonic/builders.h"
#include "api/subsonic/router.h"
#include "search/index.h"

namespace eos::api {

void add_search_handlers(HandlerTable& t) {
  t.add("search3", [](Ctx& c, Node& root) {
    const bool show_virtual = c.svc.cfg.show_virtual_albums;
    auto terms = search::query_terms(c.param("query").value_or(""));
    Loader loader(c.db, c.user.id, show_virtual);
    Node n("searchResult3");

    auto artist_count = c.int_param("artistCount", 20, 0, 500);
    auto album_count = c.int_param("albumCount", 20, 0, 500);
    // An empty query lists everything; clients that mirror the library page through
    // songs this way, so allow large pages.
    auto song_count = c.int_param("songCount", 20, 0, terms.empty() ? 5000 : 500);
    auto artist_offset = c.int_param("artistOffset", 0, 0, INT64_MAX);
    auto album_offset = c.int_param("albumOffset", 0, 0, INT64_MAX);
    auto song_offset = c.int_param("songOffset", 0, 0, INT64_MAX);

    const std::string album_artist_filter =
        "EXISTS (SELECT 1 FROM participant p JOIN album al ON al.id = p.entity_id "
        "WHERE p.entity_type = 'album' AND p.role = 'albumartist' AND p.artist_id = e.id AND " +
        visible_album_sql(show_virtual) + ")";

    if (terms.empty()) {
      if (artist_count > 0)
        n.list("artist", loader.artists(loader.ids(
                             "SELECT e.id FROM artist e WHERE " + album_artist_filter +
                                 " ORDER BY COALESCE(e.sort_name, e.name) COLLATE NOCASE, e.id LIMIT ? OFFSET ?",
                             artist_count, artist_offset)));
      if (album_count > 0)
        n.list("album", loader.albums(loader.ids(
                            "SELECT al.id FROM album al WHERE " + visible_album_sql(show_virtual) +
                                " ORDER BY COALESCE(al.sort_name, al.name) COLLATE NOCASE, al.id LIMIT ? OFFSET ?",
                            album_count, album_offset)));
      if (song_count > 0)
        n.list("song", loader.songs(loader.ids(
                           "SELECT t.id FROM track t WHERE t.missing = 0 ORDER BY t.album_id, "
                           "COALESCE(t.disc_number, 0), COALESCE(t.track_number, 0), t.path LIMIT ? OFFSET ?",
                           song_count, song_offset)));
      root.child(std::move(n));
      return;
    }

    if (artist_count > 0) {
      search::Target target{"artist", "JOIN artist e ON e.id = search_index.ref_id", album_artist_filter, "artist",
                            "COALESCE(e.sort_name, e.name)"};
      n.list("artist", loader.artists(search::find(c.db, target, terms, c.user.id, artist_count, artist_offset)));
    }
    if (album_count > 0) {
      search::Target target{"album", "JOIN album e ON e.id = search_index.ref_id",
                            visible_album_sql(show_virtual, "e"), "album", "COALESCE(e.sort_name, e.name)"};
      n.list("album", loader.albums(search::find(c.db, target, terms, c.user.id, album_count, album_offset)));
    }
    if (song_count > 0) {
      search::Target target{"track", "JOIN track e ON e.id = search_index.ref_id", "e.missing = 0", "track",
                            "COALESCE(e.sort_title, e.title)"};
      n.list("song", loader.songs(search::find(c.db, target, terms, c.user.id, song_count, song_offset)));
    }
    root.child(std::move(n));
  });
}

}  // namespace eos::api
