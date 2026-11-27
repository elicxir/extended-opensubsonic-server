// getAlbumList2, getRandomSongs, getStarred2
#include "api/subsonic/builders.h"
#include "api/subsonic/router.h"

namespace eos::api {
namespace {

constexpr const char* kVisibleAlbum = "al.missing = 0 AND al.kind = 'physical'";

}  // namespace

void add_list_handlers(HandlerTable& t) {
  t.add("getAlbumList2", [](Ctx& c, Node& root) {
    auto type = c.require("type");
    auto size = c.int_param("size", 10, 0, 500);
    auto offset = c.int_param("offset", 0, 0, INT64_MAX);
    Loader loader(c.db, c.user.id);
    const std::string base =
        std::string("SELECT al.id FROM album al LEFT JOIN annotation an ON an.user_id = ?1 "
                    "AND an.item_type = 'album' AND an.item_id = al.id WHERE ") +
        kVisibleAlbum;
    const std::string page = " LIMIT ?2 OFFSET ?3";
    std::vector<std::string> ids;
    auto by = [&](const std::string& where_order) {
      ids = loader.ids(base + where_order + page, c.user.id, size, offset);
    };
    if (type == "random") by(" ORDER BY random()");
    else if (type == "newest") by(" ORDER BY al.created_at DESC, al.id");
    else if (type == "highest") by(" AND an.rating > 0 ORDER BY an.rating DESC, al.name");
    else if (type == "frequent") by(" AND an.play_count > 0 ORDER BY an.play_count DESC, al.name");
    else if (type == "recent") by(" AND an.last_played_at IS NOT NULL ORDER BY an.last_played_at DESC");
    else if (type == "starred") by(" AND an.starred_at IS NOT NULL ORDER BY an.starred_at DESC");
    else if (type == "alphabeticalByName")
      by(" ORDER BY COALESCE(al.sort_name, al.name) COLLATE NOCASE, al.id");
    else if (type == "alphabeticalByArtist")
      by(" ORDER BY al.display_artist COLLATE NOCASE, COALESCE(al.year, 0), "
         "COALESCE(al.sort_name, al.name) COLLATE NOCASE");
    else if (type == "byYear") {
      auto from = c.opt_int("fromYear").value_or(0);
      auto to = c.opt_int("toYear").value_or(9999);
      auto lo = std::min(from, to), hi = std::max(from, to);
      ids = loader.ids(base + " AND al.year BETWEEN ?4 AND ?5 ORDER BY al.year " +
                           (from > to ? "DESC" : "ASC") + ", al.name" + page,
                       c.user.id, size, offset, lo, hi);
    } else if (type == "byGenre") {
      auto genre = c.require("genre");
      ids = loader.ids(base +
                           " AND EXISTS (SELECT 1 FROM track t JOIN track_genre tg ON tg.track_id = t.id "
                           "JOIN genre g ON g.id = tg.genre_id WHERE t.album_id = al.id AND t.missing = 0 "
                           "AND g.name = ?4 COLLATE NOCASE) ORDER BY COALESCE(al.sort_name, al.name) "
                           "COLLATE NOCASE" + page,
                       c.user.id, size, offset, genre);
    } else {
      throw SubsonicError(err::generic, "Unknown list type: " + type);
    }
    Node n("albumList2");
    n.list("album", loader.albums(ids));
    root.child(std::move(n));
  });

  t.add("getRandomSongs", [](Ctx& c, Node& root) {
    auto size = c.int_param("size", 10, 0, 500);
    auto genre = c.param("genre");
    auto from = c.opt_int("fromYear");
    auto to = c.opt_int("toYear");
    std::string sql =
        "SELECT t.id FROM track t JOIN album al ON al.id = t.album_id "
        "WHERE t.missing = 0 AND al.missing = 0";
    if (genre)
      sql += " AND EXISTS (SELECT 1 FROM track_genre tg JOIN genre g ON g.id = tg.genre_id "
             "WHERE tg.track_id = t.id AND g.name = ?2 COLLATE NOCASE)";
    if (from) sql += " AND t.year >= ?3";
    if (to) sql += " AND t.year <= ?4";
    sql += " ORDER BY random() LIMIT ?1";
    Loader loader(c.db, c.user.id);
    std::vector<std::string> ids;
    {
      auto st = c.db.prepare(sql);
      st.bind(1, size);
      if (genre) st.bind(2, *genre);
      if (from) st.bind(3, *from);
      if (to) st.bind(4, *to);
      while (st.step()) ids.push_back(st.text(0));
    }
    Node n("randomSongs");
    n.list("song", loader.songs(ids));
    root.child(std::move(n));
  });

  t.add("getStarred2", [](Ctx& c, Node& root) {
    Loader loader(c.db, c.user.id);
    auto starred = [&](const char* type) {
      return loader.ids(
          "SELECT item_id FROM annotation WHERE user_id = ? AND item_type = ? "
          "AND starred_at IS NOT NULL ORDER BY starred_at DESC",
          c.user.id, type);
    };
    Node n("starred2");
    n.list("artist", loader.artists(starred("artist")));
    n.list("album", loader.albums(starred("album")));
    n.list("song", loader.songs(starred("track")));
    root.child(std::move(n));
  });
}

}  // namespace eos::api
