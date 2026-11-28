// Read-only /ext endpoints (design: 独自拡張API / 読み取り).
#include <map>
#include <set>

#include "api/ext/model.h"
#include "api/ext/suggestions.h"
#include "api/subsonic/builders.h"
#include "core/normalize.h"
#include "search/index.h"

namespace eos::api::ext {
namespace {

std::vector<std::string> ids_of(db::Statement&& st) {
  std::vector<std::string> out;
  while (st.step()) out.push_back(st.text(0));
  return out;
}

// Tracks whose effective arrangement (highest source >= min) is `arrangement`.
std::vector<std::string> arrangement_tracks(db::Connection& db, const std::string& arrangement, int min) {
  auto st = db.prepare(
      "SELECT ta.track_id FROM track_arrangement ta JOIN track t ON t.id = ta.track_id "
      "JOIN album al ON al.id = t.album_id "
      "WHERE ta.arrangement_id = ?1 AND ta.source >= ?2 AND t.missing = 0 AND ta.source = ("
      "  SELECT MAX(source) FROM track_arrangement x WHERE x.track_id = ta.track_id AND x.source >= ?2) "
      "ORDER BY COALESCE(al.release_date, ''), al.name, COALESCE(t.disc_number, 0), COALESCE(t.track_number, 0)");
  st.bind_all(arrangement, min);
  return ids_of(std::move(st));
}

std::vector<std::string> composition_arrangements(db::Connection& db, const std::string& comp) {
  auto st = db.prepare(
      "SELECT id FROM arrangement WHERE composition_id = ? ORDER BY is_implicit DESC, COALESCE(name, '')");
  st.bind(1, comp);
  return ids_of(std::move(st));
}

std::vector<std::string> composition_tracks(db::Connection& db, const std::string& comp, int min) {
  std::vector<std::string> out;
  for (const auto& a : composition_arrangements(db, comp))
    for (auto& t : arrangement_tracks(db, a, min)) out.push_back(std::move(t));
  return out;
}

// Relations touching an item, as seen from it.
json relations_of(db::Connection& db, const std::string& type, const std::string& id, int min) {
  json out = json::array();
  auto st = db.prepare(
      "SELECT id FROM relation WHERE source >= ?3 AND ((from_type = ?1 AND from_id = ?2) OR "
      "(to_type = ?1 AND to_id = ?2)) ORDER BY type, COALESCE(position, 0), id");
  st.bind_all(type, id, min);
  for (const auto& rid : ids_of(std::move(st))) out.push_back(relation_json(db, rid, id));
  return out;
}

json target_of(ExtCtx& c, const json& relation, const std::string& self) {
  const auto& end = relation["from"]["id"] == self ? relation["to"] : relation["from"];
  json t = {{"type", end["type"]}};
  if (end["type"] == "track") {
    auto s = songs(c, {end["id"].get<std::string>()});
    t["song"] = s.empty() ? json(nullptr) : s[0];
  } else {
    t["composition"] = composition_json(c.db, end["id"].get<std::string>());
  }
  return t;
}

}  // namespace

void add_read_routes(ExtRouter& r) {
  r.add("GET", "/ext/info", [](ExtCtx&) {
    return ok({{"apiVersion", 1},
               {"serverVersion", EOS_VERSION},
               {"features",
                {"folders", "titles", "compositions", "relations", "virtualAlbums", "suggestions", "search",
                 "overrides", "apiKeys", "export", "graph", "similarity", "songDetail", "artistImages"}}});
  });

  r.add("GET", "/ext/scan", [](ExtCtx& c) {
    auto s = c.svc.scanner.status();
    json j = {{"scanning", s.scanning}, {"phase", s.phase}, {"count", s.count}, {"folderCount", s.folder_count}};
    auto st = c.db.prepare(
        "SELECT kind, started_at, finished_at, added, updated, moved, missing, errors FROM scan_run "
        "ORDER BY id DESC LIMIT 1");
    if (st.step()) {
      j["last"] = {{"kind", st.text(0)},
                   {"startedAt", iso_time(st.int64(1))},
                   {"finishedAt", st.opt_int64(2) ? json(iso_time(*st.opt_int64(2))) : json(nullptr)},
                   {"added", st.opt_int64(3).value_or(0)},
                   {"updated", st.opt_int64(4).value_or(0)},
                   {"moved", st.opt_int64(5).value_or(0)},
                   {"missing", st.opt_int64(6).value_or(0)},
                   {"errors", st.opt_int64(7).value_or(0)}};
    }
    return ok(j);
  });

  // -- folders
  r.add("GET", "/ext/folders", [](ExtCtx& c) {
    json items = json::array();
    for (const auto& id : ids_of(c.db.prepare("SELECT id FROM folder ORDER BY path_key")))
      items.push_back(folder_json(c.db, id));
    auto n = static_cast<int64_t>(items.size());
    return page(std::move(items), n);
  });
  r.add("GET", "/ext/folders/{}", [](ExtCtx& c) {
    const auto& id = c.args[0];
    json j = {{"folder", folder_json(c.db, id)}};
    json children = json::array();
    auto st = c.db.prepare("SELECT id FROM folder WHERE parent_id = ? ORDER BY sort_order, name COLLATE NOCASE");
    st.bind(1, id);
    for (const auto& child : ids_of(std::move(st))) children.push_back(folder_json(c.db, child));
    j["children"] = std::move(children);
    j["albums"] = albums(c, folder_album_ids(c.db, id));
    return ok(j);
  });

  // -- albums
  r.add("GET", "/ext/albums", [](ExtCtx& c) {
    auto kind = c.query("kind").value_or("all");
    if (kind != "all" && kind != "physical" && kind != "virtual") throw bad_request("kind must be physical, virtual or all");
    std::string where = "missing = 0" + (kind == "all" ? std::string() : " AND kind = '" + kind + "'");
    auto total = *c.db.scalar_int("SELECT COUNT(*) FROM album WHERE " + where);
    auto st = c.db.prepare("SELECT id FROM album WHERE " + where +
                           " ORDER BY COALESCE(sort_name, name) COLLATE NOCASE, id LIMIT ? OFFSET ?");
    st.bind_all(c.limit(), c.offset());
    return page(albums(c, ids_of(std::move(st))), total);
  });

  r.add("GET", "/ext/albums/{}", [](ExtCtx& c) {
    const auto& id = c.args[0];
    auto list = albums(c, {id});
    if (list.empty()) throw not_found("Album not found");
    auto j = list[0];
    Loader loader(c.db, c.user.id, true);
    j["song"] = songs(c, loader.album_track_ids(id));
    j["folderIds"] = album_folder_ids(c.db, id);
    if (j["kind"] == "physical") {
      auto st = c.db.prepare(
          "SELECT DISTINCT at.album_id FROM album_track at JOIN track t ON t.id = at.track_id "
          "WHERE t.album_id = ? AND at.excluded = 0 ORDER BY 1");
      st.bind(1, id);
      j["virtualAlbums"] = albums(c, ids_of(std::move(st)));
    } else {
      auto st = c.db.prepare(
          "SELECT DISTINCT t.album_id FROM album_track at JOIN track t ON t.id = at.track_id "
          "WHERE at.album_id = ? AND at.excluded = 0 ORDER BY 1");
      st.bind(1, id);
      j["parentAlbums"] = albums(c, ids_of(std::move(st)));
    }
    return ok(j);
  });

  r.add("GET", "/ext/albums/{}/titles", [](ExtCtx& c) {
    const auto& id = c.args[0];
    if (!exists(c.db, "album", id)) throw not_found("Album not found");
    Loader loader(c.db, c.user.id, true);
    auto tracks = loader.album_track_ids(id);
    // Groups in the order of their first track; a track with several works is in each.
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::string>> members;
    for (const auto& t : tracks) {
      auto titles = track_title_ids(c.db, t, c.min_source());
      if (titles.empty()) titles.push_back("");
      for (const auto& ti : titles) {
        if (!members.count(ti)) order.push_back(ti);
        members[ti].push_back(t);
      }
    }
    json groups = json::array();
    for (const auto& ti : order)
      groups.push_back({{"title", ti.empty() ? json(nullptr) : title_json(c.db, ti)}, {"songs", songs(c, members[ti])}});
    return ok({{"albumId", id}, {"groups", groups}});
  });

  // -- titles (works)
  r.add("GET", "/ext/titles", [](ExtCtx& c) {
    auto q = normalize(c.query("query").value_or(""));
    auto total = *c.db.scalar_int("SELECT COUNT(*) FROM title WHERE instr(name_norm, ?) > 0", q);
    auto st = c.db.prepare(
        "SELECT id FROM title WHERE instr(name_norm, ?) > 0 ORDER BY COALESCE(sort_name, name) COLLATE NOCASE "
        "LIMIT ? OFFSET ?");
    st.bind_all(q, c.limit(), c.offset());
    json items = json::array();
    for (const auto& id : ids_of(std::move(st))) {
      auto j = title_json(c.db, id);
      j["songCount"] = static_cast<int64_t>(title_track_ids(c.db, id, c.min_source()).size());
      items.push_back(std::move(j));
    }
    return page(std::move(items), total);
  });

  r.add("GET", "/ext/titles/{}", [](ExtCtx& c) {
    const auto& id = c.args[0];
    auto title = title_json(c.db, id);
    auto tracks = title_track_ids(c.db, id, c.min_source());
    std::vector<std::string> album_ids;
    std::set<std::string> seen;
    for (const auto& t : tracks) {
      auto a = *c.db.scalar_text("SELECT album_id FROM track WHERE id = ?", t);
      if (seen.insert(a).second) album_ids.push_back(a);
    }
    return ok({{"title", title}, {"albums", albums(c, album_ids)}, {"songs", songs(c, tracks)}});
  });

  // -- compositions
  r.add("GET", "/ext/compositions", [](ExtCtx& c) {
    auto q = normalize(c.query("query").value_or(""));
    auto total = *c.db.scalar_int("SELECT COUNT(*) FROM composition WHERE instr(name_norm, ?) > 0", q);
    auto st = c.db.prepare(
        "SELECT id FROM composition WHERE instr(name_norm, ?) > 0 ORDER BY name COLLATE NOCASE LIMIT ? OFFSET ?");
    st.bind_all(q, c.limit(), c.offset());
    json items = json::array();
    for (const auto& id : ids_of(std::move(st))) items.push_back(composition_json(c.db, id));
    return page(std::move(items), total);
  });

  r.add("GET", "/ext/compositions/{}", [](ExtCtx& c) {
    const auto& id = c.args[0];
    json j = {{"composition", composition_json(c.db, id)}};
    json arrangements = json::array();
    for (const auto& a : composition_arrangements(c.db, id))
      arrangements.push_back({{"arrangement", arrangement_json(c.db, a)}, {"songs", songs(c, arrangement_tracks(c.db, a, c.min_source()))}});
    j["arrangements"] = std::move(arrangements);
    j["relations"] = relations_of(c.db, "composition", id, c.min_source());
    return ok(j);
  });

  // -- songs
  r.add("GET", "/ext/songs/{}", [](ExtCtx& c) {
    require_track(c.db, c.args[0]);
    return ok(songs(c, {c.args[0]})[0]);
  });

  r.add("GET", "/ext/songs/{}/versions", [](ExtCtx& c) {
    const auto& id = c.args[0];
    require_track(c.db, id);
    const int min = c.min_source();
    json j = {{"song", songs(c, {id})[0]}};
    auto arrangement = track_arrangement(c.db, id, min);
    std::string comp;
    if (arrangement) comp = *c.db.scalar_text("SELECT composition_id FROM arrangement WHERE id = ?", *arrangement);
    j["composition"] = comp.empty() ? json(nullptr) : composition_json(c.db, comp);

    json others = json::array();
    if (!comp.empty()) {
      for (const auto& a : composition_arrangements(c.db, comp)) {
        if (a == *arrangement) continue;
        auto tracks = arrangement_tracks(c.db, a, min);
        if (!tracks.empty()) others.push_back({{"arrangement", arrangement_json(c.db, a)}, {"songs", songs(c, tracks)}});
      }
    }
    j["arrangements"] = std::move(others);

    std::vector<std::string> performances;
    if (arrangement)
      for (auto& t : arrangement_tracks(c.db, *arrangement, min))
        if (t != id) performances.push_back(std::move(t));
    j["performances"] = songs(c, performances);

    json related = json::array();
    for (auto& rel : relations_of(c.db, "track", id, min)) related.push_back({{"relation", rel}, {"target", target_of(c, rel, id)}});
    if (!comp.empty())
      for (auto& rel : relations_of(c.db, "composition", comp, min))
        related.push_back({{"relation", rel}, {"target", target_of(c, rel, comp)}});
    j["related"] = std::move(related);
    return ok(j);
  });

  // Everything known about one song, for a details screen: what it is linked to with the
  // names and where each link came from, the file, and the tags as they are in the file.
  r.add("GET", "/ext/songs/{}/detail", [](ExtCtx& c) {
    const auto& id = c.args[0];
    require_track(c.db, id);
    const int min = c.min_source();
    json j = {{"song", songs(c, {id})[0]}};
    j["album"] = albums(c, {j["song"]["albumId"].get<std::string>()})[0];

    j["composition"] = nullptr;
    j["arrangement"] = nullptr;
    j["arrangementLink"] = nullptr;
    {
      auto st = c.db.prepare(
          "SELECT ta.arrangement_id, a.composition_id, ta.source, s.method, s.score, s.rule, ta.origin, ta.origin_ref "
          "FROM track_arrangement ta JOIN arrangement a ON a.id = ta.arrangement_id "
          "LEFT JOIN suggestion s ON s.id = ta.suggestion_id "
          "WHERE ta.track_id = ? AND ta.source >= ? ORDER BY ta.source DESC LIMIT 1");
      st.bind_all(id, min);
      if (st.step()) {
        const auto arrangement = st.text(0), comp = st.text(1);
        j["arrangementLink"] = prov_fields(st.int64(2), st.opt_text(3), st, 4);
        j["arrangement"] = arrangement_json(c.db, arrangement);
        j["composition"] = composition_json(c.db, comp);
      }
    }

    // The strongest row of each work gives its link.
    json titles = json::array();
    {
      std::vector<std::pair<std::string, json>> rows;
      auto st = c.db.prepare(
          "SELECT tt.title_id, MAX(tt.source), s.method, s.score, s.rule, tt.origin, tt.origin_ref "
          "FROM track_title tt JOIN title ti ON ti.id = tt.title_id "
          "LEFT JOIN suggestion s ON s.id = tt.suggestion_id "
          "WHERE tt.track_id = ?1 AND tt.excluded = 0 AND tt.source >= ?2 AND NOT EXISTS ("
          "  SELECT 1 FROM track_title x WHERE x.track_id = tt.track_id AND x.title_id = tt.title_id "
          "  AND x.excluded = 1) GROUP BY tt.title_id ORDER BY COALESCE(ti.sort_name, ti.name), tt.title_id");
      st.bind_all(id, min);
      while (st.step()) rows.emplace_back(st.text(0), prov_fields(st.int64(1), st.opt_text(2), st, 3));
      for (auto& [title, link] : rows) titles.push_back({{"title", title_json(c.db, title)}, {"link", std::move(link)}});
    }
    j["titles"] = std::move(titles);
    // The virtual albums the song is on, with its place there. The strongest row of each
    // album gives the link.
    json virtual_albums = json::array();
    {
      struct Row {
        std::string album;
        json rest;
      };
      std::vector<Row> rows;
      auto st = c.db.prepare(
          "SELECT at.album_id, MAX(at.source), s.method, s.score, s.rule, at.origin, at.origin_ref, "
          "at.disc_number, at.track_number FROM album_track at JOIN album al ON al.id = at.album_id "
          "LEFT JOIN suggestion s ON s.id = at.suggestion_id "
          "WHERE at.track_id = ?1 AND at.excluded = 0 AND at.source >= ?2 AND al.missing = 0 AND NOT EXISTS ("
          "  SELECT 1 FROM album_track x WHERE x.track_id = at.track_id AND x.album_id = at.album_id "
          "  AND x.excluded = 1) GROUP BY at.album_id ORDER BY COALESCE(al.sort_name, al.name), at.album_id");
      st.bind_all(id, min);
      while (st.step())
        rows.push_back({st.text(0),
                        {{"link", prov_fields(st.int64(1), st.opt_text(2), st, 3)},
                         {"discNumber", st.opt_int64(7) ? json(*st.opt_int64(7)) : json(nullptr)},
                         {"trackNumber", st.opt_int64(8) ? json(*st.opt_int64(8)) : json(nullptr)}}});
      for (auto& row : rows) {
        auto found = albums(c, {row.album});
        if (found.empty()) continue;
        row.rest["album"] = std::move(found[0]);
        virtual_albums.push_back(std::move(row.rest));
      }
    }
    j["virtualAlbums"] = std::move(virtual_albums);

    auto st = c.db.prepare(
        "SELECT l.name, t.path, t.size, t.mtime, t.created_at, t.updated_at, t.audio_md5, t.has_embedded_art, "
        "t.mbz_recording_id, t.mbz_track_id, t.tags_json FROM track t JOIN library l ON l.id = t.library_id "
        "WHERE t.id = ?");
    st.bind(1, id);
    st.step();
    auto text_or_null = [&](int col) { return st.is_null(col) ? json(nullptr) : json(st.text(col)); };
    j["file"] = {{"library", st.text(0)},
                 {"path", st.text(1)},
                 {"size", st.int64(2)},
                 {"modified", iso_time(st.int64(3))},
                 {"added", iso_time(st.int64(4))},
                 {"updated", iso_time(st.int64(5))},
                 {"audioMd5", text_or_null(6)},
                 {"hasEmbeddedArt", st.int64(7) != 0},
                 {"musicBrainzRecordingId", text_or_null(8)},
                 {"musicBrainzTrackId", text_or_null(9)}};
    auto tags = json::parse(st.text(10), nullptr, false);
    j["tags"] = tags.is_object() ? std::move(tags) : json::object();
    return ok(j);
  });

  // -- search (design: 検索 / まとめて返す)
  r.add("GET", "/ext/search", [](ExtCtx& c) {
    auto terms = search::query_terms(c.query("q").value_or(c.query("query").value_or("")));
    json j = {{"compositions", json::array()}, {"titles", json::array()}, {"songs", json::array()},
              {"albums", json::array()}, {"artists", json::array()}};
    if (terms.empty()) return ok(j);
    auto limit = std::max<int64_t>(1, c.limit() == 100 ? 20 : c.limit());
    const int min = c.min_source();

    std::set<std::string> grouped;
    search::Target comp_t{"composition", "JOIN composition e ON e.id = search_index.ref_id", "", "", "e.name"};
    for (const auto& comp : search::find(c.db, comp_t, terms, c.user.id, limit, 0)) {
      auto tracks = composition_tracks(c.db, comp, min);
      grouped.insert(tracks.begin(), tracks.end());
      j["compositions"].push_back({{"composition", composition_json(c.db, comp)}, {"songs", songs(c, tracks)}});
    }
    search::Target title_t{"title", "JOIN title e ON e.id = search_index.ref_id", "", "", "COALESCE(e.sort_name, e.name)"};
    for (const auto& ti : search::find(c.db, title_t, terms, c.user.id, limit, 0)) {
      auto tracks = title_track_ids(c.db, ti, min);
      std::vector<std::string> first(tracks.begin(), tracks.begin() + std::min<size_t>(tracks.size(), 10));
      j["titles"].push_back({{"title", title_json(c.db, ti)}, {"songCount", tracks.size()}, {"songs", songs(c, first)}});
    }
    search::Target song_t{"track", "JOIN track e ON e.id = search_index.ref_id", "e.missing = 0", "track",
                          "COALESCE(e.sort_title, e.title)"};
    std::vector<std::string> song_ids;
    for (auto& s : search::find(c.db, song_t, terms, c.user.id, limit, 0))
      if (!grouped.count(s)) song_ids.push_back(std::move(s));
    j["songs"] = songs(c, song_ids);
    search::Target album_t{"album", "JOIN album e ON e.id = search_index.ref_id", "e.missing = 0", "album",
                           "COALESCE(e.sort_name, e.name)"};
    j["albums"] = albums(c, search::find(c.db, album_t, terms, c.user.id, limit, 0));
    search::Target artist_t{"artist", "JOIN artist e ON e.id = search_index.ref_id", "", "artist",
                            "COALESCE(e.sort_name, e.name)"};
    Loader loader(c.db, c.user.id, true);
    for (const auto& a : loader.artists(search::find(c.db, artist_t, terms, c.user.id, limit, 0)))
      j["artists"].push_back(to_json_value(a));
    return ok(j);
  });

  // -- suggestions
  r.add("GET", "/ext/suggestions", [](ExtCtx& c) {
    std::string where = "status = ?1";
    auto status = c.query("status").value_or("pending");
    auto rule = c.query("rule").value_or("");
    auto method = c.query("method").value_or("");
    if (!rule.empty()) where += " AND rule = ?2";
    if (!method.empty()) where += " AND method = ?3";
    auto count = c.db.prepare("SELECT COUNT(*) FROM suggestion WHERE " + where);
    count.bind(1, status);
    if (!rule.empty()) count.bind(2, rule);
    if (!method.empty()) count.bind(3, method);
    count.step();
    auto total = count.int64(0);
    auto st = c.db.prepare("SELECT id FROM suggestion WHERE " + where +
                           " ORDER BY score DESC, created_at, id LIMIT ?4 OFFSET ?5");
    st.bind(1, status);
    if (!rule.empty()) st.bind(2, rule);
    if (!method.empty()) st.bind(3, method);
    st.bind(4, c.limit());
    st.bind(5, c.offset());
    json items = json::array();
    for (const auto& id : ids_of(std::move(st))) items.push_back(suggestion_json(c.db, id));
    return page(std::move(items), total);
  });

  r.add("GET", "/ext/suggestions/{}", [](ExtCtx& c) { return ok(suggestion_json(c.db, c.args[0])); });
}

}  // namespace eos::api::ext
