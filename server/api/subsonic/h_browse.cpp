// getArtists, getArtist, getAlbum, getSong, getGenres, getIndexes, getMusicDirectory
#include <map>
#include <utf8proc.h>

#include "api/subsonic/builders.h"
#include "api/subsonic/router.h"
#include "core/strings.h"

namespace eos::api {
namespace {

// The index letter for an artist: the first character of its sort name after
// dropping ignored articles. ASCII letters are upper-cased, hiragana is folded to
// katakana, CJK and other letters are kept, everything else goes to "#".
std::string index_key(const std::string& name, const std::vector<std::string>& articles) {
  std::string s = trim(name);
  for (const auto& a : articles) {
    if (s.size() > a.size() + 1 && starts_with_ci(s, a) && s[a.size()] == ' ') {
      s = trim(s.substr(a.size() + 1));
      break;
    }
  }
  if (s.empty()) return "#";
  utf8proc_int32_t cp;
  auto n = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(s.data()),
                            static_cast<utf8proc_ssize_t>(s.size()), &cp);
  if (n <= 0) return "#";
  if (cp < 128) {
    char c = static_cast<char>(std::toupper(cp));
    return std::isalpha(static_cast<unsigned char>(c)) ? std::string(1, c) : "#";
  }
  if (cp >= 0xFF21 && cp <= 0xFF3A) cp -= 0xFF21 - 'A';  // full-width A-Z
  if (cp >= 0xFF41 && cp <= 0xFF5A) cp -= 0xFF41 - 'A';  // full-width a-z
  if (cp < 128) return std::string(1, static_cast<char>(cp));
  if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;  // hiragana -> katakana
  auto cat = utf8proc_category(cp);
  if (cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO) {
    utf8proc_uint8_t buf[4];
    auto len = utf8proc_encode_char(cp, buf);
    return std::string(reinterpret_cast<char*>(buf), static_cast<size_t>(len));
  }
  return "#";
}

// Album artists that have at least one visible album, with their index letter.
std::vector<Node> artist_index(Ctx& c, const char* artist_elem) {
  auto articles = split_any(c.svc.cfg.ignored_articles, {" "});
  Loader loader(c.db, c.user.id, c.svc.cfg.show_virtual_albums);
  auto ids = loader.ids(
      "SELECT ar.id FROM artist ar WHERE EXISTS ("
      "  SELECT 1 FROM participant p JOIN album al ON al.id = p.entity_id "
      "  WHERE p.entity_type = 'album' AND p.role = 'albumartist' AND p.artist_id = ar.id "
      "    AND " + visible_album_sql(c.svc.cfg.show_virtual_albums) + ") "
      "ORDER BY COALESCE(ar.sort_name, ar.name) COLLATE NOCASE");
  std::map<std::string, std::vector<Node>> groups;
  std::vector<std::string> order;
  auto st = c.db.prepare("SELECT COALESCE(sort_name, name) FROM artist WHERE id = ?");
  for (auto& node : loader.artists(ids, artist_elem)) {
    st.reset();
    st.bind(1, std::get<std::string>(*node.get("id")));
    std::string sort = st.step() ? st.text(0) : std::string();
    auto key = index_key(sort, articles);
    if (!groups.count(key)) order.push_back(key);
    groups[key].push_back(std::move(node));
  }
  std::sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
    if (a == "#") return false;
    if (b == "#") return true;
    return a < b;
  });
  std::vector<Node> index;
  for (const auto& key : order) {
    Node i("index");
    i.set("name", key);
    i.list(artist_elem, std::move(groups[key]));
    index.push_back(std::move(i));
  }
  return index;
}

constexpr const char* kUnfiled = "fo-unfiled";
constexpr const char* kUnfiledName = "（未分類）";
// The album `al` is in a folder (af): a tag/manual row not cancelled by an excluded row.
constexpr const char* kAlbumFolders =
    "SELECT 1 FROM album_folder af WHERE af.album_id = al.id AND af.excluded = 0 AND NOT EXISTS ("
    "SELECT 1 FROM album_folder x WHERE x.album_id = af.album_id AND x.folder_id = af.folder_id AND x.excluded = 1)";

// A directory child (Child with isDir=true) for an album, made from its AlbumID3.
Node album_as_child(const Node& album, const std::string& parent) {
  Node n("child");
  auto copy = [&](const char* from, const char* to) {
    if (auto v = album.get(from)) n.set(to, std::visit([](const auto& x) { return Value(x); }, *v));
  };
  copy("id", "id");
  n.set("parent", parent).set("isDir", true);
  copy("name", "title");
  copy("name", "album");
  copy("artist", "artist");
  copy("year", "year");
  copy("genre", "genre");
  copy("coverArt", "coverArt");
  copy("created", "created");
  copy("starred", "starred");
  copy("playCount", "playCount");
  copy("artistId", "artistId");
  return n;
}

}  // namespace

void add_browse_handlers(HandlerTable& t) {
  t.add("getArtists", [](Ctx& c, Node& root) {
    Node n("artists");
    n.set("ignoredArticles", c.svc.cfg.ignored_articles);
    n.list("index", artist_index(c, "artist"));
    root.child(std::move(n));
  });

  t.add("getArtist", [](Ctx& c, Node& root) {
    Loader loader(c.db, c.user.id, c.svc.cfg.show_virtual_albums);
    auto id = c.require("id");
    auto artist = loader.artist(id);
    if (!artist) throw SubsonicError(err::not_found, "Artist not found");
    artist->list("album", loader.albums(loader.artist_album_ids(id)));
    root.child(std::move(*artist));
  });

  t.add("getAlbum", [](Ctx& c, Node& root) {
    Loader loader(c.db, c.user.id, c.svc.cfg.show_virtual_albums);
    auto id = c.require("id");
    auto album = loader.album(id);
    if (!album) throw SubsonicError(err::not_found, "Album not found");
    album->list("song", loader.songs(loader.album_track_ids(id)));
    root.child(std::move(*album));
  });

  t.add("getSong", [](Ctx& c, Node& root) {
    Loader loader(c.db, c.user.id, c.svc.cfg.show_virtual_albums);
    auto song = loader.song(c.require("id"));
    if (!song) throw SubsonicError(err::not_found, "Song not found");
    root.child(std::move(*song));
  });

  t.add("getGenres", [](Ctx& c, Node& root) {
    std::vector<Node> genres;
    auto st = c.db.prepare(
        "SELECT g.name, COUNT(DISTINCT t.id), COUNT(DISTINCT t.album_id) FROM genre g "
        "JOIN track_genre tg ON tg.genre_id = g.id JOIN track t ON t.id = tg.track_id "
        "WHERE t.missing = 0 GROUP BY g.id ORDER BY g.name COLLATE NOCASE");
    while (st.step()) {
      Node g("genre");
      g.set("songCount", st.int64(1)).set("albumCount", st.int64(2)).text(st.text(0));
      genres.push_back(std::move(g));
    }
    Node n("genres");
    n.list("genre", std::move(genres));
    root.child(std::move(n));
  });

  // Folder-style browsing over the virtual folders (design: getIndexes /
  // getMusicDirectory). Folder ids are "fo-<id>"; albums in no folder sit under
  // "fo-unfiled". A library without folder tags falls back to artists -> albums.
  t.add("getIndexes", [](Ctx& c, Node& root) {
    Node n("indexes");
    n.set("ignoredArticles", c.svc.cfg.ignored_articles);
    n.set("lastModified", c.db.scalar_int("SELECT COALESCE(MAX(last_scan_at), 0) FROM library").value_or(0));
    if (!c.db.scalar_int("SELECT 1 FROM folder LIMIT 1")) {
      n.list("index", artist_index(c, "artist"));
      root.child(std::move(n));
      return;
    }
    auto articles = split_any(c.svc.cfg.ignored_articles, {" "});
    std::map<std::string, std::vector<Node>> groups;
    auto st = c.db.prepare("SELECT id, name FROM folder WHERE parent_id IS NULL ORDER BY name COLLATE NOCASE");
    while (st.step())
      groups[index_key(st.text(1), articles)].push_back(Node("artist").set("id", "fo-" + st.text(0)).set("name", st.text(1)));
    if (c.db.scalar_int("SELECT 1 FROM album al WHERE al.missing = 0 AND NOT EXISTS (" + std::string(kAlbumFolders) +
                        ") LIMIT 1"))
      groups["#"].push_back(Node("artist").set("id", kUnfiled).set("name", kUnfiledName));
    std::vector<Node> index;
    for (auto& [key, items] : groups) {
      if (key == "#") continue;
      index.push_back(Node("index").set("name", key).list("artist", std::move(items)));
    }
    if (groups.count("#")) index.push_back(Node("index").set("name", "#").list("artist", std::move(groups["#"])));
    n.list("index", std::move(index));
    root.child(std::move(n));
  });

  t.add("getMusicDirectory", [](Ctx& c, Node& root) {
    Loader loader(c.db, c.user.id, true);  // folders show virtual albums too
    auto id = c.require("id");
    Node dir("directory");
    std::vector<Node> children;
    if (id == kUnfiled) {
      dir.set("id", id).set("name", kUnfiledName);
      auto albums = loader.ids("SELECT al.id FROM album al WHERE al.missing = 0 AND NOT EXISTS (" +
                               std::string(kAlbumFolders) + ") ORDER BY COALESCE(al.sort_name, al.name) COLLATE NOCASE");
      for (const auto& a : loader.albums(albums)) children.push_back(album_as_child(a, id));
    } else if (id.starts_with("fo-")) {
      auto folder = id.substr(3);
      auto st = c.db.prepare("SELECT name, parent_id FROM folder WHERE id = ?");
      st.bind(1, folder);
      if (!st.step()) throw SubsonicError(err::not_found, "Directory not found");
      dir.set("id", id).set("name", st.text(0));
      if (!st.is_null(1)) dir.set("parent", "fo-" + st.text(1));
      auto sub = c.db.prepare("SELECT id, name FROM folder WHERE parent_id = ? ORDER BY sort_order, name COLLATE NOCASE");
      sub.bind(1, folder);
      while (sub.step())
        children.push_back(Node("child").set("id", "fo-" + sub.text(0)).set("parent", id).set("isDir", true)
                               .set("title", sub.text(1)));
      auto albums = loader.ids(
          "SELECT al.id FROM album al WHERE al.missing = 0 AND EXISTS (" + std::string(kAlbumFolders) +
              " AND af.folder_id = ?) ORDER BY COALESCE(al.year, 0), COALESCE(al.sort_name, al.name) COLLATE NOCASE",
          folder);
      for (const auto& a : loader.albums(albums)) children.push_back(album_as_child(a, id));
    } else if (auto album = loader.album(id)) {
      dir.set("id", id).set("name", std::get<std::string>(*album->get("name")));
      auto folder = c.db.scalar_text(
          "SELECT af.folder_id FROM album_folder af WHERE af.album_id = ? AND af.excluded = 0 AND NOT EXISTS ("
          "SELECT 1 FROM album_folder x WHERE x.album_id = af.album_id AND x.folder_id = af.folder_id "
          "AND x.excluded = 1) LIMIT 1",
          id);
      if (folder)
        dir.set("parent", "fo-" + *folder);
      else if (auto artist = album->get("artistId"))
        dir.set("parent", std::get<std::string>(*artist));
      children = loader.songs(loader.album_track_ids(id), "child");
    } else if (auto artist = loader.artist(id)) {
      dir.set("id", id).set("name", std::get<std::string>(*artist->get("name")));
      for (const auto& a : loader.albums(loader.artist_album_ids(id))) children.push_back(album_as_child(a, id));
    } else {
      throw SubsonicError(err::not_found, "Directory not found");
    }
    dir.list("child", std::move(children));
    root.child(std::move(dir));
  });
}

}  // namespace eos::api
