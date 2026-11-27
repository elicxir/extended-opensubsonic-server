// scrobble, star, unstar, setRating
#include "api/subsonic/router.h"

namespace eos::api {
namespace {

// What kind of item an id names: track, album or artist. Throws 70 when none.
std::string item_type_of(db::Connection& db, const std::string& id) {
  if (db.scalar_int("SELECT 1 FROM track WHERE id = ?", id)) return "track";
  if (db.scalar_int("SELECT 1 FROM album WHERE id = ?", id)) return "album";
  if (db.scalar_int("SELECT 1 FROM artist WHERE id = ?", id)) return "artist";
  throw SubsonicError(err::not_found, "Item not found: " + id);
}

void ensure_annotation(db::Connection& w, int64_t user, const std::string& type, const std::string& id) {
  w.run("INSERT OR IGNORE INTO annotation (user_id, item_type, item_id) VALUES (?, ?, ?)", user, type, id);
}

void set_star(Ctx& c, bool on) {
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto& id : c.params("id")) items.emplace_back(item_type_of(c.db, id), id);
  for (const auto& id : c.params("albumId")) items.emplace_back("album", id);
  for (const auto& id : c.params("artistId")) items.emplace_back("artist", id);
  if (items.empty()) throw SubsonicError(err::missing_param, "Required parameter is missing: id");
  auto now = db::now_ms();
  c.write([&](db::Connection& w) {
    db::Transaction tx(w);
    for (const auto& [type, id] : items) {
      ensure_annotation(w, c.user.id, type, id);
      if (on)
        w.run("UPDATE annotation SET starred_at = ? WHERE user_id = ? AND item_type = ? AND item_id = ?",
              now, c.user.id, type, id);
      else
        w.run("UPDATE annotation SET starred_at = NULL WHERE user_id = ? AND item_type = ? AND item_id = ?",
              c.user.id, type, id);
    }
    tx.commit();
  });
}

}  // namespace

void add_annotation_handlers(HandlerTable& t) {
  t.add("star", [](Ctx& c, Node&) { set_star(c, true); });
  t.add("unstar", [](Ctx& c, Node&) { set_star(c, false); });

  t.add("setRating", [](Ctx& c, Node&) {
    auto id = c.require("id");
    auto rating = c.opt_int("rating");
    if (!rating) throw SubsonicError(err::missing_param, "Required parameter is missing: rating");
    if (*rating < 0 || *rating > 5) throw SubsonicError(err::generic, "rating must be between 0 and 5");
    auto type = item_type_of(c.db, id);
    c.write([&](db::Connection& w) {
      ensure_annotation(w, c.user.id, type, id);
      w.run("UPDATE annotation SET rating = ? WHERE user_id = ? AND item_type = ? AND item_id = ?",
            *rating == 0 ? std::optional<int64_t>() : rating, c.user.id, type, id);
    });
  });

  t.add("scrobble", [](Ctx& c, Node&) {
    auto ids = c.params("id");
    if (ids.empty()) throw SubsonicError(err::missing_param, "Required parameter is missing: id");
    if (!c.bool_param("submission", true)) return;  // "now playing" (no getNowPlaying yet)
    auto times = c.params("time");
    struct Play {
      std::string track, album;
      int64_t at;
    };
    std::vector<Play> plays;
    for (size_t i = 0; i < ids.size(); ++i) {
      auto album = c.db.scalar_text("SELECT album_id FROM track WHERE id = ?", ids[i]);
      if (!album) throw SubsonicError(err::not_found, "Song not found: " + ids[i]);
      int64_t at = db::now_ms();
      if (i < times.size()) {
        try {
          at = std::stoll(times[i]);
        } catch (...) {
        }
      }
      plays.push_back({ids[i], *album, at});
    }
    c.write([&](db::Connection& w) {
      db::Transaction tx(w);
      for (const auto& p : plays) {
        for (const auto& [type, id] : {std::pair<std::string, std::string>{"track", p.track}, {"album", p.album}}) {
          ensure_annotation(w, c.user.id, type, id);
          w.run(
              "UPDATE annotation SET play_count = play_count + 1, "
              "last_played_at = MAX(COALESCE(last_played_at, 0), ?) "
              "WHERE user_id = ? AND item_type = ? AND item_id = ?",
              p.at, c.user.id, type, id);
        }
        w.run("INSERT INTO scrobble (user_id, track_id, played_at, device_id) VALUES (?, ?, ?, ?)",
              c.user.id, p.track, p.at, c.client.empty() ? std::optional<std::string>() : c.client);
      }
      tx.commit();
    });
  });
}

}  // namespace eos::api
