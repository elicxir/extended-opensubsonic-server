// GET /ext/export and POST /ext/import: manual and accepted data as JSON, for backups
// and for moving to a fresh database (design: 非機能要件 / 書き出しと読み込み). Tracks are
// referred to by id plus pid, path and audio MD5, so they can be found again after a
// rescan even when their ids differ.
#include <map>
#include <set>

#include "api/ext/model.h"
#include "core/ids.h"
#include "scanner/derived.h"

namespace eos::api::ext {
namespace {

using njson = nlohmann::json;

njson opt_json(const std::optional<std::string>& v) { return v ? njson(*v) : njson(nullptr); }
njson opt_json(const std::optional<int64_t>& v) { return v ? njson(*v) : njson(nullptr); }

njson names(db::Connection& db, const char* type, const std::string& id, const char* role) {
  njson out = njson::array();
  auto st = db.prepare(
      "SELECT a.name FROM participant p JOIN artist a ON a.id = p.artist_id WHERE p.entity_type = ? "
      "AND p.entity_id = ? AND p.role = ? AND p.source IN (1, 3) ORDER BY p.position");
  st.bind_all(type, id, role);
  while (st.step()) out.push_back(st.text(0));
  return out;
}

// Adds "origin" and "originRef" (columns `col` and `col + 1`) when the row has them.
njson with_origin(njson j, db::Statement& s, int col) {
  if (auto o = s.opt_text(col)) j["origin"] = *o;
  if (auto r = s.opt_text(col + 1)) j["originRef"] = *r;
  return j;
}

// The origin of an imported row; an unknown word is dropped rather than refused.
std::optional<std::string> origin_in(const njson& r) {
  if (!r.contains("origin") || !r["origin"].is_string()) return std::nullopt;
  auto o = r["origin"].get<std::string>();
  return valid_origin(o) ? std::optional(o) : std::nullopt;
}
std::optional<std::string> origin_ref_in(const njson& r) {
  return r.contains("originRef") && r["originRef"].is_string() ? std::optional(r["originRef"].get<std::string>())
                                                                 : std::nullopt;
}

njson export_all(db::Connection& db) {
  njson out = {{"format", "eos-export"}, {"version", 1}, {"exportedAt", iso_time(db::now_ms())}};
  std::set<std::string> tracks, albums, compositions, arrangements, titles, folders;

  auto rows = [&](const std::string& sql, auto&& fn) {
    njson arr = njson::array();
    auto st = db.prepare(sql);
    while (st.step()) arr.push_back(fn(st));
    return arr;
  };

  out["trackArrangements"] = rows(
      "SELECT track_id, arrangement_id, source, origin, origin_ref FROM track_arrangement WHERE source IN (1, 3)",
      [&](db::Statement& s) {
        tracks.insert(s.text(0));
        arrangements.insert(s.text(1));
        return with_origin(njson{{"track", s.text(0)}, {"arrangement", s.text(1)}, {"source", s.int64(2)}}, s, 3);
      });
  out["trackTitles"] = rows(
      "SELECT track_id, title_id, source, excluded, origin, origin_ref FROM track_title WHERE source IN (1, 3)",
      [&](db::Statement& s) {
        tracks.insert(s.text(0));
        titles.insert(s.text(1));
        return with_origin(njson{{"track", s.text(0)}, {"title", s.text(1)}, {"source", s.int64(2)},
                                 {"excluded", s.int64(3) != 0}},
                           s, 4);
      });
  out["albumFolders"] = rows(
      "SELECT album_id, folder_id, source, excluded, origin, origin_ref FROM album_folder WHERE source IN (1, 3)",
      [&](db::Statement& s) {
        albums.insert(s.text(0));
        folders.insert(s.text(1));
        return with_origin(njson{{"album", s.text(0)}, {"folder", s.text(1)}, {"source", s.int64(2)},
                                 {"excluded", s.int64(3) != 0}},
                           s, 4);
      });
  out["albumTracks"] = rows(
      "SELECT album_id, track_id, disc_number, track_number, source, excluded, origin, origin_ref FROM album_track "
      "WHERE source IN (1, 3)",
      [&](db::Statement& s) {
        albums.insert(s.text(0));
        tracks.insert(s.text(1));
        return with_origin(njson{{"album", s.text(0)},
                                 {"track", s.text(1)},
                                 {"disc", opt_json(s.opt_int64(2))},
                                 {"trackNumber", opt_json(s.opt_int64(3))},
                                 {"source", s.int64(4)},
                                 {"excluded", s.int64(5) != 0}},
                           s, 6);
      });
  out["relations"] = rows(
      "SELECT id, from_type, from_id, to_type, to_id, type, subtype, position, note, source, origin, origin_ref "
      "FROM relation WHERE source IN (1, 3)",
      [&](db::Statement& s) {
        for (auto [t, i] : {std::pair{s.text(1), s.text(2)}, {s.text(3), s.text(4)}})
          (t == "track" ? tracks : compositions).insert(i);
        return with_origin(njson{{"id", s.text(0)},
                                 {"from", {{"type", s.text(1)}, {"id", s.text(2)}}},
                                 {"to", {{"type", s.text(3)}, {"id", s.text(4)}}},
                                 {"type", s.text(5)},
                                 {"subtype", opt_json(s.opt_text(6))},
                                 {"position", opt_json(s.opt_int64(7))},
                                 {"note", opt_json(s.opt_text(8))},
                                 {"source", s.int64(9)}},
                           s, 10);
      });
  out["overrides"] = rows("SELECT entity_type, entity_id, field, value FROM field_override", [&](db::Statement& s) {
    (s.text(0) == "track" ? tracks : albums).insert(s.text(1));
    return njson{{"entityType", s.text(0)}, {"entityId", s.text(1)}, {"field", s.text(2)}, {"value", opt_json(s.opt_text(3))}};
  });
  out["preferred"] = rows(
      "SELECT u.username, p.composition_id, p.track_id FROM preferred_version p JOIN user u ON u.id = p.user_id",
      [&](db::Statement& s) {
        compositions.insert(s.text(1));
        tracks.insert(s.text(2));
        return njson{{"user", s.text(0)}, {"composition", s.text(1)}, {"track", s.text(2)}};
      });
  out["suggestions"] = rows(
      "SELECT rule, method, model, fingerprint, payload, score, status, evidence, origin, origin_ref FROM suggestion "
      "WHERE status IN ('accepted', 'rejected', 'auto_applied')",
      [&](db::Statement& s) {
        return with_origin(njson{{"rule", s.text(0)},
                                 {"method", s.text(1)},
                                 {"model", opt_json(s.opt_text(2))},
                                 {"fingerprint", s.text(3)},
                                 {"payload", s.text(4)},
                                 {"score", s.real(5)},
                                 {"status", s.text(6)},
                                 {"evidence", opt_json(s.opt_text(7))}},
                           s, 8);
      });

  // Entities: every manual / accepted one, plus tag-made ones the rows above refer to.
  for (const auto& a : arrangements)
    if (auto comp = db.scalar_text("SELECT composition_id FROM arrangement WHERE id = ?", a)) compositions.insert(*comp);
  {
    auto st = db.prepare("SELECT id FROM arrangement WHERE source IN (1, 3)");
    while (st.step()) arrangements.insert(st.text(0));
    auto sc = db.prepare("SELECT id FROM composition WHERE source IN (1, 3)");
    while (sc.step()) compositions.insert(sc.text(0));
    auto stt = db.prepare("SELECT id FROM title WHERE source IN (1, 3)");
    while (stt.step()) titles.insert(stt.text(0));
    auto sf = db.prepare("SELECT id FROM folder WHERE source IN (1, 3)");
    while (sf.step()) folders.insert(sf.text(0));
  }
  // Parents of exported folders come too.
  for (std::vector<std::string> todo(folders.begin(), folders.end()); !todo.empty();) {
    auto f = todo.back();
    todo.pop_back();
    if (auto p = db.scalar_text("SELECT parent_id FROM folder WHERE id = ?", f))
      if (folders.insert(*p).second) todo.push_back(*p);
  }

  auto one = [&](const std::string& sql, const std::string& id, auto&& fn) {
    auto st = db.prepare(sql);
    st.bind(1, id);
    return st.step() ? fn(st) : njson(nullptr);
  };
  njson comps = njson::array(), arrs = njson::array(), tis = njson::array(), fos = njson::array(),
        valbums = njson::array(), track_refs = njson::object(), album_refs = njson::object();
  for (const auto& id : compositions)
    if (auto j = one("SELECT id, name, mbz_work_id, source FROM composition WHERE id = ?", id, [&](db::Statement& s) {
          return njson{{"id", s.text(0)}, {"name", s.text(1)}, {"mbzWorkId", opt_json(s.opt_text(2))},
                       {"source", s.int64(3)}, {"composers", names(db, "composition", id, "composer")}};
        });
        !j.is_null())
      comps.push_back(j);
  for (const auto& id : arrangements)
    if (auto j = one("SELECT id, composition_id, name, kind, is_implicit, source FROM arrangement WHERE id = ?", id,
                     [&](db::Statement& s) {
                       return njson{{"id", s.text(0)},       {"compositionId", s.text(1)},
                                    {"name", opt_json(s.opt_text(2))}, {"kind", s.text(3)},
                                    {"isImplicit", s.int64(4) != 0},   {"source", s.int64(5)},
                                    {"arrangers", names(db, "arrangement", id, "arranger")}};
                     });
        !j.is_null())
      arrs.push_back(j);
  for (const auto& id : titles)
    if (auto j = one("SELECT id, name, sort_name, kind, year, source FROM title WHERE id = ?", id, [&](db::Statement& s) {
          return njson{{"id", s.text(0)}, {"name", s.text(1)}, {"sortName", opt_json(s.opt_text(2))},
                       {"kind", opt_json(s.opt_text(3))}, {"year", opt_json(s.opt_int64(4))}, {"source", s.int64(5)}};
        });
        !j.is_null())
      tis.push_back(j);
  for (const auto& id : folders)
    if (auto j = one("SELECT id, parent_id, name, path_key, sort_order, source FROM folder WHERE id = ?", id,
                     [&](db::Statement& s) {
                       return njson{{"id", s.text(0)},      {"parentId", opt_json(s.opt_text(1))},
                                    {"name", s.text(2)},    {"pathKey", s.text(3)},
                                    {"sortOrder", opt_json(s.opt_int64(4))}, {"source", s.int64(5)}};
                     });
        !j.is_null())
      fos.push_back(j);
  // Folder rows must be created parents first.
  std::sort(fos.begin(), fos.end(), [](const njson& a, const njson& b) {
    return a["pathKey"].get<std::string>().size() < b["pathKey"].get<std::string>().size();
  });
  for (const auto& id : albums) {
    auto st = db.prepare("SELECT pid, name, kind, sort_name, display_artist, source FROM album WHERE id = ?");
    st.bind(1, id);
    if (!st.step()) continue;
    if (st.text(2) == "virtual") {
      valbums.push_back({{"id", id},     {"name", st.text(1)}, {"sortName", opt_json(st.opt_text(3))},
                         {"displayArtist", opt_json(st.opt_text(4))}, {"source", st.int64(5)}});
    } else {
      album_refs[id] = {{"pid", st.text(0)}, {"name", st.text(1)}};
    }
  }
  {
    auto st = db.prepare("SELECT id FROM album WHERE kind = 'virtual' AND source IN (1, 3)");
    std::set<std::string> listed;
    for (const auto& v : valbums) listed.insert(v["id"]);
    while (st.step())
      if (!listed.count(st.text(0))) {
        auto id = st.text(0);
        auto s2 = db.prepare("SELECT name, sort_name, display_artist, source FROM album WHERE id = ?");
        s2.bind(1, id);
        s2.step();
        valbums.push_back({{"id", id}, {"name", s2.text(0)}, {"sortName", opt_json(s2.opt_text(1))},
                           {"displayArtist", opt_json(s2.opt_text(2))}, {"source", s2.int64(3)}});
      }
  }
  for (const auto& id : tracks) {
    auto st = db.prepare("SELECT pid, path, audio_md5 FROM track WHERE id = ?");
    st.bind(1, id);
    if (st.step()) track_refs[id] = {{"pid", st.text(0)}, {"path", st.text(1)}, {"audioMd5", opt_json(st.opt_text(2))}};
  }
  out["tracks"] = track_refs;
  out["albums"] = album_refs;
  out["compositions"] = comps;
  out["arrangements"] = arrs;
  out["titles"] = tis;
  out["folders"] = fos;
  out["virtualAlbums"] = valbums;
  return out;
}

struct ImportResult {
  std::map<std::string, int64_t> counts;
  int64_t unmatched_tracks = 0, unmatched_albums = 0;
};

ImportResult import_all(db::Connection& w, const njson& in) {
  if (in.value("format", "") != "eos-export" || in.value("version", 0) != 1)
    throw bad_request("Not an eos export (format eos-export, version 1)");
  ImportResult res;
  const auto now = db::now_ms();

  // Map exported ids to local ones.
  std::map<std::string, std::string> track_map, album_map, title_map, folder_map;
  const auto track_refs = in.value("tracks", njson::object());
  for (const auto& [id, ref] : track_refs.items()) {
    std::optional<std::string> local;
    if (w.scalar_int("SELECT 1 FROM track WHERE id = ?", id)) local = id;
    auto unique = [&](const char* sql, const std::string& v) -> std::optional<std::string> {
      if (v.empty()) return std::nullopt;
      auto st = w.prepare(sql);
      st.bind(1, v);
      std::optional<std::string> hit;
      while (st.step()) {
        if (hit) return std::nullopt;
        hit = st.text(0);
      }
      return hit;
    };
    if (!local) local = unique("SELECT id FROM track WHERE pid = ? AND missing = 0", ref.value("pid", ""));
    if (!local && ref["audioMd5"].is_string())
      local = unique("SELECT id FROM track WHERE audio_md5 = ? AND missing = 0", ref["audioMd5"].get<std::string>());
    if (!local) local = unique("SELECT id FROM track WHERE path = ?", ref.value("path", ""));
    if (local) track_map[id] = *local;
    else ++res.unmatched_tracks;
  }
  const auto album_refs = in.value("albums", njson::object());
  for (const auto& [id, ref] : album_refs.items()) {
    std::optional<std::string> local;
    if (w.scalar_int("SELECT 1 FROM album WHERE id = ?", id)) local = id;
    if (!local) local = w.scalar_text("SELECT id FROM album WHERE pid = ? AND kind = 'physical'", ref.value("pid", ""));
    if (local) album_map[id] = *local;
    else ++res.unmatched_albums;
  }

  auto count = [&](const char* key) { ++res.counts[key]; };
  for (const auto& c : in.value("compositions", njson::array())) {
    w.run(
        "INSERT OR IGNORE INTO composition (id, name, name_norm, mbz_work_id, source, created_at, updated_at) "
        "VALUES (?, ?, eos_norm(?), ?, ?, ?, ?)",
        c["id"].get<std::string>(), c["name"].get<std::string>(), c["name"].get<std::string>(),
        c["mbzWorkId"].is_string() ? std::optional(c["mbzWorkId"].get<std::string>()) : std::nullopt,
        c["source"].get<int64_t>(), now, now);
    if (w.changes()) count("compositions");
    int pos = 0;
    for (const auto& n : c.value("composers", njson::array())) {
      scanner::Person p{n.get<std::string>(), "", "", ""};
      scanner::upsert_artist(w, p);
      w.run(
          "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source) "
          "VALUES ('composition', ?, ?, 'composer', '', ?, 3)",
          c["id"].get<std::string>(), scanner::artist_id(p.name), pos++);
    }
  }
  for (const auto& a : in.value("arrangements", njson::array())) {
    w.run(
        "INSERT OR IGNORE INTO arrangement (id, composition_id, name, kind, is_implicit, source, created_at, updated_at) "
        "SELECT ?, ?, ?, ?, ?, ?, ?, ? WHERE EXISTS (SELECT 1 FROM composition WHERE id = ?2)",
        a["id"].get<std::string>(), a["compositionId"].get<std::string>(),
        a["name"].is_string() ? std::optional(a["name"].get<std::string>()) : std::nullopt, a["kind"].get<std::string>(),
        a["isImplicit"].get<bool>(), a["source"].get<int64_t>(), now, now);
    if (w.changes()) count("arrangements");
    int pos = 0;
    for (const auto& n : a.value("arrangers", njson::array())) {
      scanner::Person p{n.get<std::string>(), "", "", ""};
      scanner::upsert_artist(w, p);
      w.run(
          "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source) "
          "VALUES ('arrangement', ?, ?, 'arranger', '', ?, 3)",
          a["id"].get<std::string>(), scanner::artist_id(p.name), pos++);
    }
  }
  for (const auto& t : in.value("titles", njson::array())) {
    auto id = t["id"].get<std::string>();
    auto existing = w.scalar_text("SELECT id FROM title WHERE id = ? OR name_norm = eos_norm(?)", id,
                                  t["name"].get<std::string>());
    if (existing) {
      title_map[id] = *existing;
      continue;
    }
    w.run(
        "INSERT INTO title (id, name, sort_name, name_norm, kind, year, source, created_at, updated_at) "
        "VALUES (?, ?, ?, eos_norm(?), ?, ?, ?, ?, ?)",
        id, t["name"].get<std::string>(),
        t["sortName"].is_string() ? std::optional(t["sortName"].get<std::string>()) : std::nullopt,
        t["name"].get<std::string>(), t["kind"].is_string() ? std::optional(t["kind"].get<std::string>()) : std::nullopt,
        t["year"].is_number_integer() ? std::optional(t["year"].get<int64_t>()) : std::nullopt,
        t["source"].get<int64_t>(), now, now);
    title_map[id] = id;
    count("titles");
  }
  for (const auto& f : in.value("folders", njson::array())) {
    auto id = f["id"].get<std::string>();
    auto existing = w.scalar_text("SELECT id FROM folder WHERE id = ? OR path_key = ?", id, f["pathKey"].get<std::string>());
    if (existing) {
      folder_map[id] = *existing;
      continue;
    }
    std::optional<std::string> parent;
    if (f["parentId"].is_string()) {
      auto it = folder_map.find(f["parentId"].get<std::string>());
      if (it != folder_map.end()) parent = it->second;
    }
    w.run("INSERT INTO folder (id, parent_id, name, path_key, sort_order, source) VALUES (?, ?, ?, ?, ?, ?)", id, parent,
          f["name"].get<std::string>(), f["pathKey"].get<std::string>(),
          f["sortOrder"].is_number_integer() ? std::optional(f["sortOrder"].get<int64_t>()) : std::nullopt,
          f["source"].get<int64_t>());
    folder_map[id] = id;
    count("folders");
  }
  auto lib = w.scalar_int("SELECT MIN(id) FROM library").value_or(1);
  for (const auto& v : in.value("virtualAlbums", njson::array())) {
    auto id = v["id"].get<std::string>();
    if (!w.scalar_int("SELECT 1 FROM album WHERE id = ?", id)) {
      w.run(
          "INSERT INTO album (id, pid, library_id, name, sort_name, display_artist, kind, source, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, 'virtual', ?, ?, ?)",
          id, id, lib, v["name"].get<std::string>(),
          v["sortName"].is_string() ? std::optional(v["sortName"].get<std::string>()) : std::nullopt,
          v["displayArtist"].is_string() ? std::optional(v["displayArtist"].get<std::string>()) : std::nullopt,
          v["source"].get<int64_t>(), now, now);
      count("virtualAlbums");
    }
    album_map[id] = id;
  }

  auto mapped = [](const std::map<std::string, std::string>& m, const njson& id) -> std::optional<std::string> {
    if (!id.is_string()) return std::nullopt;
    auto it = m.find(id.get<std::string>());
    return it == m.end() ? std::nullopt : std::optional(it->second);
  };
  for (const auto& r : in.value("trackArrangements", njson::array())) {
    auto t = mapped(track_map, r["track"]);
    if (!t) continue;
    w.run(
        "INSERT OR REPLACE INTO track_arrangement (track_id, arrangement_id, source, origin, origin_ref) "
        "SELECT ?, ?, ?, ?, ? WHERE EXISTS (SELECT 1 FROM arrangement WHERE id = ?2)",
        *t, r["arrangement"].get<std::string>(), r["source"].get<int64_t>(), origin_in(r), origin_ref_in(r));
    if (w.changes()) count("trackArrangements");
  }
  for (const auto& r : in.value("trackTitles", njson::array())) {
    auto t = mapped(track_map, r["track"]);
    auto ti = mapped(title_map, r["title"]);
    if (!t || !ti) continue;
    w.run(
        "INSERT OR REPLACE INTO track_title (track_id, title_id, source, excluded, origin, origin_ref) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        *t, *ti, r["source"].get<int64_t>(), r["excluded"].get<bool>(), origin_in(r), origin_ref_in(r));
    count("trackTitles");
  }
  for (const auto& r : in.value("albumFolders", njson::array())) {
    auto a = mapped(album_map, r["album"]);
    auto f = mapped(folder_map, r["folder"]);
    if (!a || !f) continue;
    w.run(
        "INSERT OR REPLACE INTO album_folder (album_id, folder_id, source, excluded, origin, origin_ref) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        *a, *f, r["source"].get<int64_t>(), r["excluded"].get<bool>(), origin_in(r), origin_ref_in(r));
    count("albumFolders");
  }
  for (const auto& r : in.value("albumTracks", njson::array())) {
    auto a = mapped(album_map, r["album"]);
    auto t = mapped(track_map, r["track"]);
    if (!a || !t) continue;
    w.run(
        "INSERT OR REPLACE INTO album_track (album_id, track_id, disc_number, track_number, source, excluded, "
        "origin, origin_ref) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        *a, *t, r["disc"].is_number_integer() ? std::optional(r["disc"].get<int64_t>()) : std::nullopt,
        r["trackNumber"].is_number_integer() ? std::optional(r["trackNumber"].get<int64_t>()) : std::nullopt,
        r["source"].get<int64_t>(), r["excluded"].get<bool>(), origin_in(r), origin_ref_in(r));
    count("albumTracks");
  }
  for (const auto& r : in.value("relations", njson::array())) {
    auto end = [&](const njson& ref) -> std::optional<std::string> {
      if (ref["type"] == "track") return mapped(track_map, ref["id"]);
      auto id = ref["id"].get<std::string>();
      return w.scalar_int("SELECT 1 FROM composition WHERE id = ?", id) ? std::optional(id) : std::nullopt;
    };
    auto from = end(r["from"]), to = end(r["to"]);
    if (!from || !to) continue;
    w.run(
        "INSERT OR IGNORE INTO relation (id, from_type, from_id, to_type, to_id, type, subtype, position, note, source, "
        "created_at, updated_at, origin, origin_ref) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ids::random_id(), r["from"]["type"].get<std::string>(), *from, r["to"]["type"].get<std::string>(), *to,
        r["type"].get<std::string>(), r["subtype"].is_string() ? std::optional(r["subtype"].get<std::string>()) : std::nullopt,
        r["position"].is_number_integer() ? std::optional(r["position"].get<int64_t>()) : std::nullopt,
        r["note"].is_string() ? std::optional(r["note"].get<std::string>()) : std::nullopt, r["source"].get<int64_t>(), now,
        now, origin_in(r), origin_ref_in(r));
    if (w.changes()) count("relations");
  }
  for (const auto& o : in.value("overrides", njson::array())) {
    auto type = o["entityType"].get<std::string>();
    auto id = mapped(type == "track" ? track_map : album_map, o["entityId"]);
    if (!id) continue;
    w.run("INSERT OR REPLACE INTO field_override (entity_type, entity_id, field, value, updated_at) VALUES (?, ?, ?, ?, ?)",
          type, *id, o["field"].get<std::string>(),
          o["value"].is_string() ? std::optional(o["value"].get<std::string>()) : std::nullopt, now);
    count("overrides");
  }
  for (const auto& p : in.value("preferred", njson::array())) {
    auto user = w.scalar_int("SELECT id FROM user WHERE username = ?", p["user"].get<std::string>());
    auto t = mapped(track_map, p["track"]);
    if (!user || !t) continue;
    w.run(
        "INSERT OR REPLACE INTO preferred_version (user_id, composition_id, track_id) "
        "SELECT ?, ?, ? WHERE EXISTS (SELECT 1 FROM composition WHERE id = ?2)",
        *user, p["composition"].get<std::string>(), *t);
    if (w.changes()) count("preferred");
  }
  for (const auto& s : in.value("suggestions", njson::array())) {
    w.run(
        "INSERT INTO suggestion (id, rule, method, model, evidence, fingerprint, payload, score, status, "
        "created_at, decided_at, origin, origin_ref) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        // A decision overrides a still-open copy that a scan raised in the meantime.
        "ON CONFLICT(fingerprint) DO UPDATE SET status = excluded.status, decided_at = excluded.decided_at "
        "WHERE suggestion.status IN ('pending', 'stale')",
        ids::random_id(), s["rule"].get<std::string>(), s["method"].get<std::string>(),
        s["model"].is_string() ? std::optional(s["model"].get<std::string>()) : std::nullopt,
        s["evidence"].is_string() ? std::optional(s["evidence"].get<std::string>()) : std::nullopt,
        s["fingerprint"].get<std::string>(), s["payload"].get<std::string>(), s["score"].get<double>(),
        s["status"].get<std::string>(), now, now, origin_in(s), origin_ref_in(s));
    if (w.changes()) count("suggestions");
  }
  // Overrides take effect on the live columns now; the scanner keeps them afterwards.
  w.exec(
      "UPDATE track SET title = (SELECT value FROM field_override o WHERE o.entity_type = 'track' "
      "  AND o.entity_id = track.id AND o.field = 'title' AND o.value IS NOT NULL) "
      "WHERE id IN (SELECT entity_id FROM field_override WHERE entity_type = 'track' AND field = 'title' "
      "  AND value IS NOT NULL);"
      "UPDATE album SET name = (SELECT value FROM field_override o WHERE o.entity_type = 'album' "
      "  AND o.entity_id = album.id AND o.field = 'name' AND o.value IS NOT NULL) "
      "WHERE id IN (SELECT entity_id FROM field_override WHERE entity_type = 'album' AND field = 'name' "
      "  AND value IS NOT NULL);");
  scanner::finish_custom(w, now);
  return res;
}

}  // namespace

void add_transfer_routes(ExtRouter& r) {
  r.add("GET", "/ext/export", [](ExtCtx& c) { return ok(export_all(c.db)); }, Access::admin);

  r.add(
      "POST", "/ext/import",
      [](ExtCtx& c) {
        const auto& in = c.body();
        auto res = c.write([&](db::Connection& w) { return import_all(w, in); }, true);
        json counts = json::object();
        for (const auto& [k, v] : res.counts) counts[k] = v;
        return ok({{"imported", counts},
                   {"unmatched", {{"tracks", res.unmatched_tracks}, {"albums", res.unmatched_albums}}}});
      },
      Access::admin);
}

}  // namespace eos::api::ext
