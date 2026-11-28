#include "api/ext/model.h"

#include <format>
#include <map>
#include <unordered_map>

#include "api/subsonic/builders.h"

namespace eos::api::ext {
namespace {

std::vector<std::string> collect(db::Statement&& st) {
  std::vector<std::string> out;
  while (st.step()) out.push_back(st.text(0));
  return out;
}

json people(db::Connection& db, const char* type, const std::string& id, const char* role) {
  json out = json::array();
  auto st = db.prepare(
      "SELECT a.id, a.name, p.source FROM participant p JOIN artist a ON a.id = p.artist_id "
      "WHERE p.entity_type = ? AND p.entity_id = ? AND p.role = ? AND p.source = ("
      "  SELECT MAX(source) FROM participant q WHERE q.entity_type = p.entity_type "
      "  AND q.entity_id = p.entity_id AND q.role = p.role) ORDER BY p.position");
  st.bind_all(type, id, role);
  while (st.step()) out.push_back({{"id", st.text(0)}, {"name", st.text(1)}});
  return out;
}

}  // namespace

std::string provenance(int64_t source, const std::optional<std::string>& method) {
  if (source >= 3) return "manual";
  if (source == 2) return "tag";
  return method.value_or("rule");
}

std::optional<std::string> origin_of(int64_t source, const std::optional<std::string>& origin) {
  if (source >= 3) return "manual";
  if (source == 2) return "tag";
  return origin;
}

bool valid_origin(const std::string& origin) {
  return origin == "external" || origin == "audio" || origin == "name" || origin == "llm";
}

bool exists(db::Connection& db, const char* table, const std::string& id) {
  return db.scalar_int(std::format("SELECT 1 FROM {} WHERE id = ?", table), id).has_value();
}

void require_track(db::Connection& db, const std::string& id) {
  if (!db.scalar_int("SELECT 1 FROM track WHERE id = ? AND missing = 0", id)) throw not_found("Song not found: " + id);
}

int64_t source_of(db::Connection& db, const char* table, const std::string& id) {
  auto s = db.scalar_int(std::format("SELECT source FROM {} WHERE id = ?", table), id);
  if (!s) throw not_found(std::string(table) + " not found");
  return *s;
}

std::vector<std::string> track_title_ids(db::Connection& db, const std::string& track, int min_source) {
  auto st = db.prepare(
      "SELECT DISTINCT tt.title_id FROM track_title tt WHERE tt.track_id = ? AND tt.excluded = 0 "
      "AND tt.source >= ? AND NOT EXISTS (SELECT 1 FROM track_title x WHERE x.track_id = tt.track_id "
      "AND x.title_id = tt.title_id AND x.excluded = 1) ORDER BY tt.title_id");
  st.bind_all(track, min_source);
  return collect(std::move(st));
}

std::vector<std::string> title_track_ids(db::Connection& db, const std::string& title, int min_source) {
  auto st = db.prepare(
      "SELECT DISTINCT t.id FROM track_title tt JOIN track t ON t.id = tt.track_id "
      "JOIN album al ON al.id = t.album_id "
      "WHERE tt.title_id = ? AND tt.excluded = 0 AND tt.source >= ? AND t.missing = 0 AND NOT EXISTS ("
      "  SELECT 1 FROM track_title x WHERE x.track_id = tt.track_id AND x.title_id = tt.title_id "
      "  AND x.excluded = 1) "
      "ORDER BY COALESCE(al.release_date, ''), al.name, COALESCE(t.disc_number, 0), "
      "COALESCE(t.track_number, 0), t.path");
  st.bind_all(title, min_source);
  return collect(std::move(st));
}

std::vector<std::string> folder_album_ids(db::Connection& db, const std::string& folder) {
  auto st = db.prepare(
      "SELECT DISTINCT af.album_id FROM album_folder af JOIN album al ON al.id = af.album_id "
      "WHERE af.folder_id = ? AND af.excluded = 0 AND al.missing = 0 AND NOT EXISTS ("
      "  SELECT 1 FROM album_folder x WHERE x.album_id = af.album_id AND x.folder_id = af.folder_id "
      "  AND x.excluded = 1) ORDER BY COALESCE(al.year, 0), COALESCE(al.sort_name, al.name)");
  st.bind(1, folder);
  return collect(std::move(st));
}

std::vector<std::string> album_folder_ids(db::Connection& db, const std::string& album) {
  auto st = db.prepare(
      "SELECT DISTINCT af.folder_id FROM album_folder af WHERE af.album_id = ? AND af.excluded = 0 "
      "AND NOT EXISTS (SELECT 1 FROM album_folder x WHERE x.album_id = af.album_id "
      "AND x.folder_id = af.folder_id AND x.excluded = 1) ORDER BY af.folder_id");
  st.bind(1, album);
  return collect(std::move(st));
}

std::optional<std::string> track_arrangement(db::Connection& db, const std::string& track, int min_source) {
  return db.scalar_text(
      "SELECT arrangement_id FROM track_arrangement WHERE track_id = ? AND source >= ? "
      "ORDER BY source DESC LIMIT 1",
      track, min_source);
}

json songs(ExtCtx& c, const std::vector<std::string>& ids) {
  json out = json::array();
  if (ids.empty()) return out;
  Loader loader(c.db, c.user.id, true);
  auto nodes = loader.songs(ids);
  const auto js = json_ids(ids);
  const int min = c.min_source();

  struct Extra {
    std::string arrangement, composition, prov;
    std::vector<std::string> titles, virtual_albums;
    std::optional<std::string> performance_kind, recorded_date;
  };
  std::unordered_map<std::string, Extra> extra;
  {
    auto st = c.db.prepare(
        "SELECT ta.track_id, ta.arrangement_id, a.composition_id, ta.source, s.method "
        "FROM track_arrangement ta JOIN arrangement a ON a.id = ta.arrangement_id "
        "LEFT JOIN suggestion s ON s.id = ta.suggestion_id "
        "WHERE ta.track_id IN (SELECT value FROM json_each(?1)) AND ta.source >= ?2 "
        "AND ta.source = (SELECT MAX(source) FROM track_arrangement x WHERE x.track_id = ta.track_id "
        "AND x.source >= ?2)");
    st.bind_all(js, min);
    while (st.step())
      extra[st.text(0)] = {st.text(1), st.text(2), provenance(st.int64(3), st.opt_text(4)), {}, {}, {}, {}};
  }
  {
    auto st = c.db.prepare(
        "SELECT DISTINCT tt.track_id, tt.title_id FROM track_title tt "
        "WHERE tt.track_id IN (SELECT value FROM json_each(?1)) AND tt.excluded = 0 AND tt.source >= ?2 "
        "AND NOT EXISTS (SELECT 1 FROM track_title x WHERE x.track_id = tt.track_id "
        "AND x.title_id = tt.title_id AND x.excluded = 1) ORDER BY 1, 2");
    st.bind_all(js, min);
    while (st.step()) extra[st.text(0)].titles.push_back(st.text(1));
  }
  {
    auto st = c.db.prepare(
        "SELECT DISTINCT at.track_id, at.album_id FROM album_track at "
        "WHERE at.track_id IN (SELECT value FROM json_each(?1)) AND at.excluded = 0 AND at.source >= ?2 "
        "AND NOT EXISTS (SELECT 1 FROM album_track x WHERE x.track_id = at.track_id "
        "AND x.album_id = at.album_id AND x.excluded = 1) ORDER BY 1, 2");
    st.bind_all(js, min);
    while (st.step()) extra[st.text(0)].virtual_albums.push_back(st.text(1));
  }
  {
    auto st = c.db.prepare(
        "SELECT id, performance_kind, recorded_date FROM track WHERE id IN (SELECT value FROM json_each(?))");
    st.bind(1, js);
    while (st.step()) {
      auto& e = extra[st.text(0)];
      e.performance_kind = st.opt_text(1);
      e.recorded_date = st.opt_text(2);
    }
  }
  for (const auto& n : nodes) {
    auto j = to_json_value(n);
    const auto& e = extra[j["id"].get<std::string>()];
    j["compositionId"] = e.composition.empty() ? json(nullptr) : json(e.composition);
    j["arrangementId"] = e.arrangement.empty() ? json(nullptr) : json(e.arrangement);
    j["arrangementProvenance"] = e.prov.empty() ? json(nullptr) : json(e.prov);
    j["titleIds"] = e.titles;
    j["virtualAlbumIds"] = e.virtual_albums;
    j["performanceKind"] = e.performance_kind ? json(*e.performance_kind) : json(nullptr);
    j["recordedDate"] = e.recorded_date ? json(*e.recorded_date) : json(nullptr);
    out.push_back(std::move(j));
  }
  return out;
}

json albums(ExtCtx& c, const std::vector<std::string>& ids) {
  json out = json::array();
  if (ids.empty()) return out;
  Loader loader(c.db, c.user.id, true);
  std::unordered_map<std::string, std::pair<std::string, int64_t>> kinds;
  auto st = c.db.prepare("SELECT id, kind, source FROM album WHERE id IN (SELECT value FROM json_each(?))");
  st.bind(1, json_ids(ids));
  while (st.step()) kinds[st.text(0)] = {st.text(1), st.int64(2)};
  for (const auto& n : loader.albums(ids)) {
    auto j = to_json_value(n);
    const auto& k = kinds[j["id"].get<std::string>()];
    j["kind"] = k.first;
    j["provenance"] = provenance(k.second, std::nullopt);
    out.push_back(std::move(j));
  }
  return out;
}

json title_json(db::Connection& db, const std::string& id) {
  auto st = db.prepare("SELECT id, name, sort_name, kind, year, source FROM title WHERE id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Title not found");
  return {{"id", st.text(0)},
          {"name", st.text(1)},
          {"sortName", st.opt_text(2) ? json(*st.opt_text(2)) : json(nullptr)},
          {"kind", st.opt_text(3) ? json(*st.opt_text(3)) : json(nullptr)},
          {"year", st.opt_int64(4) ? json(*st.opt_int64(4)) : json(nullptr)},
          {"provenance", provenance(st.int64(5), std::nullopt)}};
}

json composition_json(db::Connection& db, const std::string& id) {
  auto st = db.prepare(
      "SELECT co.id, co.name, co.mbz_work_id, co.source, s.method FROM composition co "
      "LEFT JOIN suggestion s ON s.id = co.suggestion_id WHERE co.id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Composition not found");
  return {{"id", st.text(0)},
          {"name", st.text(1)},
          {"musicBrainzWorkId", st.opt_text(2) ? json(*st.opt_text(2)) : json(nullptr)},
          {"composers", people(db, "composition", id, "composer")},
          {"provenance", provenance(st.int64(3), st.opt_text(4))}};
}

json arrangement_json(db::Connection& db, const std::string& id) {
  auto st = db.prepare(
      "SELECT ar.id, ar.composition_id, ar.name, ar.kind, ar.is_implicit, ar.source, s.method "
      "FROM arrangement ar LEFT JOIN suggestion s ON s.id = ar.suggestion_id WHERE ar.id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Arrangement not found");
  return {{"id", st.text(0)},
          {"compositionId", st.text(1)},
          {"name", st.opt_text(2) ? json(*st.opt_text(2)) : json(nullptr)},
          {"kind", st.text(3)},
          {"isImplicit", st.int64(4) != 0},
          {"arrangers", people(db, "arrangement", id, "arranger")},
          {"provenance", provenance(st.int64(5), st.opt_text(6))}};
}

json relation_json(db::Connection& db, const std::string& id, const std::string& seen_from) {
  auto st = db.prepare(
      "SELECT r.id, r.from_type, r.from_id, r.to_type, r.to_id, r.type, r.subtype, r.position, r.note, "
      "r.source, s.method, s.score FROM relation r LEFT JOIN suggestion s ON s.id = r.suggestion_id "
      "WHERE r.id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Relation not found");
  json j = {{"id", st.text(0)},
            {"from", {{"type", st.text(1)}, {"id", st.text(2)}}},
            {"to", {{"type", st.text(3)}, {"id", st.text(4)}}},
            {"type", st.text(5)},
            {"subtype", st.opt_text(6) ? json(*st.opt_text(6)) : json(nullptr)},
            {"position", st.opt_int64(7) ? json(*st.opt_int64(7)) : json(nullptr)},
            {"note", st.opt_text(8) ? json(*st.opt_text(8)) : json(nullptr)},
            {"provenance", provenance(st.int64(9), st.opt_text(10))}};
  if (st.int64(9) == 1 && !st.is_null(11)) j["confidence"] = st.real(11);
  if (!seen_from.empty()) j["direction"] = st.text(2) == seen_from ? "out" : "in";
  return j;
}

json folder_json(db::Connection& db, const std::string& id) {
  auto st = db.prepare(
      "SELECT f.id, f.name, f.parent_id, f.sort_order, f.source, "
      "(SELECT COUNT(*) FROM folder c WHERE c.parent_id = f.id) FROM folder f WHERE f.id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Folder not found");
  return {{"id", st.text(0)},
          {"name", st.text(1)},
          {"parentId", st.opt_text(2) ? json(*st.opt_text(2)) : json(nullptr)},
          {"sortOrder", st.opt_int64(3) ? json(*st.opt_int64(3)) : json(nullptr)},
          {"albumCount", static_cast<int64_t>(folder_album_ids(db, id).size())},
          {"childCount", st.int64(5)},
          {"provenance", provenance(st.int64(4), std::nullopt)}};
}

}  // namespace eos::api::ext
