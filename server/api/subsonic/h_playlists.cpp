// getPlaylists, getPlaylist, createPlaylist, updatePlaylist, deletePlaylist
#include <algorithm>
#include <set>

#include "api/subsonic/builders.h"
#include "api/subsonic/router.h"
#include "core/ids.h"

namespace eos::api {
namespace {

struct PlaylistRow {
  std::string id, name, comment, owner;
  int64_t owner_id = 0;
  bool is_public = false;
  int64_t created = 0, changed = 0, song_count = 0, duration_ms = 0;
};

std::optional<PlaylistRow> load_playlist(db::Connection& db, const std::string& id) {
  auto st = db.prepare(
      "SELECT p.id, p.name, COALESCE(p.comment, ''), u.username, p.owner_id, p.public, p.created_at, "
      "p.updated_at, "
      "(SELECT COUNT(*) FROM playlist_entry e JOIN track t ON t.id = e.track_id "
      " WHERE e.playlist_id = p.id AND t.missing = 0), "
      "(SELECT COALESCE(SUM(t.duration_ms), 0) FROM playlist_entry e JOIN track t ON t.id = e.track_id "
      " WHERE e.playlist_id = p.id AND t.missing = 0) "
      "FROM playlist p JOIN user u ON u.id = p.owner_id WHERE p.id = ?");
  st.bind(1, id);
  if (!st.step()) return std::nullopt;
  return PlaylistRow{st.text(0),  st.text(1),       st.text(2),  st.text(3),  st.int64(4),
                     st.int64(5) != 0, st.int64(6), st.int64(7), st.int64(8), st.int64(9)};
}

Node playlist_node(const PlaylistRow& p) {
  Node n("playlist");
  n.set("id", p.id)
      .set("name", p.name)
      .set("comment", p.comment)
      .set("owner", p.owner)
      .set("public", p.is_public)
      .set("songCount", p.song_count)
      .set("duration", (p.duration_ms + 500) / 1000)
      .set("created", iso_time(p.created))
      .set("changed", iso_time(p.changed));
  return n;
}

PlaylistRow visible_playlist(Ctx& c, const std::string& id) {
  auto p = load_playlist(c.db, id);
  if (!p || (p->owner_id != c.user.id && !p->is_public))
    throw SubsonicError(err::not_found, "Playlist not found");
  return *p;
}

PlaylistRow owned_playlist(Ctx& c, const std::string& id) {
  auto p = visible_playlist(c, id);
  if (p.owner_id != c.user.id) throw SubsonicError(err::not_authorized, "Only the owner can change a playlist");
  return p;
}

void check_tracks(Ctx& c, const std::vector<std::string>& ids) {
  for (const auto& id : ids)
    if (!c.db.scalar_int("SELECT 1 FROM track WHERE id = ?", id))
      throw SubsonicError(err::not_found, "Song not found: " + id);
}

std::vector<std::string> entries(db::Connection& db, const std::string& playlist_id) {
  std::vector<std::string> out;
  auto st = db.prepare("SELECT track_id FROM playlist_entry WHERE playlist_id = ? ORDER BY position");
  st.bind(1, playlist_id);
  while (st.step()) out.push_back(st.text(0));
  return out;
}

void write_entries(db::Connection& w, const std::string& playlist_id, const std::vector<std::string>& tracks) {
  w.run("DELETE FROM playlist_entry WHERE playlist_id = ?", playlist_id);
  int64_t pos = 0;
  for (const auto& t : tracks)
    w.run("INSERT INTO playlist_entry (playlist_id, position, track_id) VALUES (?, ?, ?)", playlist_id,
          pos++, t);
}

void add_full_playlist(Ctx& c, Node& root, const std::string& id) {
  auto p = visible_playlist(c, id);
  auto n = playlist_node(p);
  Loader loader(c.db, c.user.id);
  n.list("entry", loader.songs(entries(c.db, id), "entry"));
  root.child(std::move(n));
}

}  // namespace

void add_playlist_handlers(HandlerTable& t) {
  t.add("getPlaylists", [](Ctx& c, Node& root) {
    auto username = c.param("username");
    if (username && *username != c.user.username) c.require_admin();
    std::vector<Node> lists;
    auto st = c.db.prepare(
        "SELECT p.id FROM playlist p JOIN user u ON u.id = p.owner_id "
        "WHERE (?1 IS NULL AND (p.owner_id = ?2 OR p.public = 1)) OR (?1 IS NOT NULL AND u.username = ?1) "
        "ORDER BY p.name COLLATE NOCASE");
    st.bind_all(username, c.user.id);
    std::vector<std::string> ids;
    while (st.step()) ids.push_back(st.text(0));
    for (const auto& id : ids)
      if (auto p = load_playlist(c.db, id)) lists.push_back(playlist_node(*p));
    Node n("playlists");
    n.list("playlist", std::move(lists));
    root.child(std::move(n));
  });

  t.add("getPlaylist", [](Ctx& c, Node& root) { add_full_playlist(c, root, c.require("id")); });

  t.add("createPlaylist", [](Ctx& c, Node& root) {
    auto songs = c.params("songId");
    check_tracks(c, songs);
    auto existing = c.param("playlistId");
    std::string id;
    if (existing) {
      owned_playlist(c, *existing);
      id = *existing;
      c.write([&](db::Connection& w) {
        db::Transaction tx(w);
        if (auto name = c.param("name")) w.run("UPDATE playlist SET name = ? WHERE id = ?", *name, id);
        w.run("UPDATE playlist SET updated_at = ? WHERE id = ?", db::now_ms(), id);
        write_entries(w, id, songs);
        tx.commit();
      });
    } else {
      auto name = c.require("name");
      id = ids::random_id();
      c.write([&](db::Connection& w) {
        db::Transaction tx(w);
        auto now = db::now_ms();
        w.run("INSERT INTO playlist (id, owner_id, name, created_at, updated_at) VALUES (?, ?, ?, ?, ?)", id,
              c.user.id, name, now, now);
        write_entries(w, id, songs);
        tx.commit();
      });
    }
    add_full_playlist(c, root, id);
  });

  t.add("updatePlaylist", [](Ctx& c, Node&) {
    auto id = c.require("playlistId");
    owned_playlist(c, id);
    auto add = c.params("songIdToAdd");
    check_tracks(c, add);
    std::set<int64_t, std::greater<>> remove;
    for (const auto& s : c.params("songIndexToRemove")) {
      try {
        remove.insert(std::stoll(s));
      } catch (...) {
        throw SubsonicError(err::generic, "songIndexToRemove must be an integer");
      }
    }
    auto tracks = entries(c.db, id);
    for (auto idx : remove)  // largest index first, so earlier indexes stay valid
      if (idx >= 0 && idx < static_cast<int64_t>(tracks.size())) tracks.erase(tracks.begin() + idx);
    tracks.insert(tracks.end(), add.begin(), add.end());
    c.write([&](db::Connection& w) {
      db::Transaction tx(w);
      if (auto name = c.param("name")) w.run("UPDATE playlist SET name = ? WHERE id = ?", *name, id);
      if (auto comment = c.param("comment")) w.run("UPDATE playlist SET comment = ? WHERE id = ?", *comment, id);
      if (c.param("public")) w.run("UPDATE playlist SET public = ? WHERE id = ?", c.bool_param("public", false), id);
      w.run("UPDATE playlist SET updated_at = ? WHERE id = ?", db::now_ms(), id);
      write_entries(w, id, tracks);
      tx.commit();
    });
  });

  t.add("deletePlaylist", [](Ctx& c, Node&) {
    auto id = c.require("id");
    owned_playlist(c, id);
    c.write([&](db::Connection& w) { w.run("DELETE FROM playlist WHERE id = ?", id); });
  });
}

}  // namespace eos::api
