#include "api/ext/suggestions.h"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>

#include "api/ext/model.h"
#include "api/node.h"
#include "core/crypto.h"
#include "core/ids.h"
#include "core/normalize.h"
#include "core/strings.h"
#include "scanner/derived.h"
#include "scanner/track_info.h"
#include "search/index.h"

namespace eos::api::ext {
namespace {

using njson = nlohmann::json;

const std::set<std::string> kRelationTypes = {"medley", "quote", "derivative", "cover", "related"};

std::string str(const njson& j, const char* key) {
  if (!j.contains(key) || !j[key].is_string()) throw bad_request(std::format("proposal.{} must be a string", key));
  return j[key].get<std::string>();
}

std::vector<std::string> str_list(const njson& j, const char* key) {
  if (!j.contains(key) || !j[key].is_array() || j[key].empty())
    throw bad_request(std::format("proposal.{} must be a non-empty array", key));
  std::vector<std::string> out;
  for (const auto& v : j[key]) {
    if (!v.is_string()) throw bad_request(std::format("proposal.{} must hold strings", key));
    out.push_back(v.get<std::string>());
  }
  return out;
}

void check_ref(db::Connection& db, const njson& ref, const char* what) {
  if (!ref.is_object() || !ref.contains("type") || !ref.contains("id"))
    throw bad_request(std::format("proposal.{} must be {{type, id}}", what));
  auto type = ref["type"].get<std::string>();
  auto id = ref["id"].get<std::string>();
  if (type == "track") require_track(db, id);
  else if (type == "composition") {
    if (!exists(db, "composition", id)) throw not_found("Composition not found: " + id);
  } else throw bad_request(std::format("proposal.{}.type must be track or composition", what));
}

std::string track_label(db::Connection& db, const std::string& id) {
  auto st = db.prepare("SELECT t.title, al.name FROM track t JOIN album al ON al.id = t.album_id WHERE t.id = ?");
  st.bind(1, id);
  return st.step() ? std::format("{} ({})", st.text(0), st.text(1)) : id;
}

std::string ref_label(db::Connection& db, const njson& ref) {
  auto id = ref.value("id", "");
  if (ref.value("type", "") == "composition")
    return db.scalar_text("SELECT name FROM composition WHERE id = ?", id).value_or(id);
  return track_label(db, id);
}

std::string summary(db::Connection& db, const njson& p) {
  auto action = p.value("action", "");
  if (action == "link_composition")
    return std::format("{}曲を楽曲「{}」にまとめる", p["tracks"].size(), p.value("name", "?"));
  if (action == "merge_arrangements") return std::format("{}曲を同じ編曲にまとめる", p["tracks"].size());
  if (action == "relation") {
    auto sub = p.contains("subtype") && p["subtype"].is_string() ? "（" + p["subtype"].get<std::string>() + "）" : "";
    return std::format("「{}」→「{}」: {}{}", ref_label(db, p["from"]), ref_label(db, p["to"]), p.value("type", "?"), sub);
  }
  if (action == "medley")
    return std::format("「{}」は{}曲のメドレー", ref_label(db, p["from"]), p["parts"].size());
  if (action == "link_title") return std::format("{}曲を作品「{}」に入れる", p["tracks"].size(), p.value("titleName", p.value("titleId", "?")));
  if (action == "virtual_album") return std::format("仮想アルバム「{}」に{}曲を入れる", p.value("name", "?"), p["tracks"].size());
  if (action == "link_arrangement") {
    auto comp = p["composition"].value("name", std::string("?"));
    auto arr = p.contains("arrangement") && p["arrangement"].is_object() && p["arrangement"].contains("name") &&
                       p["arrangement"]["name"].is_string()
                   ? "編曲「" + p["arrangement"]["name"].get<std::string>() + "」"
                   : std::string("原曲");
    auto text = std::format("{}曲を楽曲「{}」の{}に入れる", p["tracks"].size(), comp, arr);
    if (p.contains("originalTracks") && p["originalTracks"].is_array() && !p["originalTracks"].empty())
      text += std::format("（{}曲を原曲に）", p["originalTracks"].size());
    return text;
  }
  return action;
}

json targets(db::Connection& db, const njson& p) {
  json out = json::array();
  std::set<std::string> seen;
  auto add = [&](const std::string& type, const std::string& id) {
    if (id.empty() || !seen.insert(type + id).second) return;
    std::string name = type == "track" ? track_label(db, id)
                                       : db.scalar_text("SELECT name FROM composition WHERE id = ?", id).value_or(id);
    out.push_back({{"type", type}, {"id", id}, {"name", name}});
  };
  for (const char* k : {"tracks", "originalTracks"})
    if (p.contains(k) && p[k].is_array())
      for (const auto& t : p[k]) add("track", t.get<std::string>());
  for (const char* k : {"from", "to"})
    if (p.contains(k) && p[k].is_object()) add(p[k].value("type", ""), p[k].value("id", ""));
  if (p.contains("parts"))
    for (const auto& part : p["parts"]) add(part["to"].value("type", ""), part["to"].value("id", ""));
  if (p.contains("compositionId") && p["compositionId"].is_string()) add("composition", p["compositionId"]);
  if (p.contains("composition") && p["composition"].is_object() && p["composition"].contains("id"))
    add("composition", p["composition"]["id"].get<std::string>());
  return out;
}

void insert_relation(db::Connection& w, const njson& from, const njson& to, const std::string& type,
                     const njson& subtype, const njson& position, const njson& note, int source,
                     const std::string& suggestion_id) {
  auto now = db::now_ms();
  w.run(
      "INSERT OR IGNORE INTO relation (id, from_type, from_id, to_type, to_id, type, subtype, position, note, "
      "source, suggestion_id, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
      ids::random_id(), from["type"].get<std::string>(), from["id"].get<std::string>(), to["type"].get<std::string>(),
      to["id"].get<std::string>(), type, subtype.is_string() ? std::optional(subtype.get<std::string>()) : std::nullopt,
      position.is_number_integer() ? std::optional(position.get<int64_t>()) : std::nullopt,
      note.is_string() ? std::optional(note.get<std::string>()) : std::nullopt, source,
      suggestion_id.empty() ? std::optional<std::string>() : suggestion_id, now, now);
}

std::optional<std::string> opt(const std::string& s) { return s.empty() ? std::nullopt : std::optional(s); }

// Effective arrangers of a track: the rows of the strongest source (tag ARRANGER, or a
// manual edit), in order.
std::vector<std::string> track_arrangers(db::Connection& w, const std::string& track) {
  auto st = w.prepare(
      "SELECT a.name FROM participant p JOIN artist a ON a.id = p.artist_id "
      "WHERE p.entity_type = 'track' AND p.entity_id = ?1 AND p.role = 'arranger' AND p.source = ("
      "  SELECT MAX(source) FROM participant q WHERE q.entity_type = 'track' AND q.entity_id = ?1 "
      "  AND q.role = 'arranger') ORDER BY p.position");
  st.bind(1, track);
  std::vector<std::string> out;
  while (st.step()) out.push_back(st.text(0));
  return out;
}

// Where a track goes in a composition when the proposal names no arrangement: the
// original (implicit arrangement), or, when the track has arrangers, the unnamed
// arrangement by exactly those arrangers (made when missing). Matches what the scanner
// does for WORK + ARRANGER tags.
class ArrangementPicker {
 public:
  ArrangementPicker(db::Connection& w, std::string comp, int source, std::optional<std::string> sugg, int64_t now)
      : w_(w), comp_(std::move(comp)), source_(source), sugg_(std::move(sugg)), now_(now) {}

  std::string for_track(const std::string& track) {
    auto arrangers = track_arrangers(w_, track);
    if (arrangers.empty()) return implicit();
    std::vector<std::string> norm;
    for (const auto& n : arrangers) norm.push_back(normalize(n));
    std::sort(norm.begin(), norm.end());
    norm.erase(std::unique(norm.begin(), norm.end()), norm.end());
    auto key = join(norm, ",");
    if (auto it = by_arrangers_.find(key); it != by_arrangers_.end()) return it->second;
    auto id = w_.scalar_text(
                    "SELECT ar.id FROM arrangement ar WHERE ar.composition_id = ? AND ar.is_implicit = 0 "
                    "AND ar.name IS NULL AND (SELECT group_concat(n, ',') FROM (SELECT DISTINCT eos_norm(a.name) AS n "
                    "  FROM participant p JOIN artist a ON a.id = p.artist_id WHERE p.entity_type = 'arrangement' "
                    "  AND p.entity_id = ar.id AND p.role = 'arranger' ORDER BY n)) = ? LIMIT 1",
                    comp_, key)
                  .value_or("");
    if (id.empty()) {
      id = ids::random_id();
      w_.run(
          "INSERT INTO arrangement (id, composition_id, kind, is_implicit, source, suggestion_id, created_at, "
          "updated_at) VALUES (?, ?, ?, 0, ?, ?, ?, ?)",
          id, comp_, scanner::arrangement_kind(""), source_, sugg_, now_, now_);
      int pos = 0;
      for (const auto& n : arrangers) {
        scanner::Person person{n, "", "", ""};
        scanner::upsert_artist(w_, person);
        w_.run(
            "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source, "
            "suggestion_id) VALUES ('arrangement', ?, ?, 'arranger', '', ?, ?, ?)",
            id, scanner::artist_id(person.name), pos++, source_, sugg_);
      }
    }
    by_arrangers_[key] = id;
    return id;
  }

  std::string implicit() {
    if (!implicit_.empty()) return implicit_;
    implicit_ = w_.scalar_text("SELECT id FROM arrangement WHERE composition_id = ? AND is_implicit = 1", comp_)
                    .value_or("");
    if (implicit_.empty()) {
      implicit_ = ids::random_id();
      w_.run(
          "INSERT INTO arrangement (id, composition_id, kind, is_implicit, source, suggestion_id, created_at, "
          "updated_at) VALUES (?, ?, 'original', 1, ?, ?, ?, ?)",
          implicit_, comp_, source_, sugg_, now_, now_);
    }
    return implicit_;
  }

 private:
  db::Connection& w_;
  std::string comp_;
  int source_;
  std::optional<std::string> sugg_;
  int64_t now_;
  std::string implicit_;
  std::map<std::string, std::string> by_arrangers_;
};

// Links each track at `source`, replacing its link of that source. The arrangements the
// tracks leave are added to `left` (see prune_left).
void link_tracks(db::Connection& w, const njson& tracks, const std::function<std::string(const std::string&)>& arrangement,
                 int source, const std::optional<std::string>& sugg, std::set<std::string>& left) {
  for (const auto& t : tracks) {
    auto id = t.get<std::string>();
    auto target = arrangement(id);
    if (auto before = w.scalar_text("SELECT arrangement_id FROM track_arrangement WHERE track_id = ? AND source = ?",
                                    id, source);
        before && *before != target)
      left.insert(*before);
    w.run(
        "INSERT OR REPLACE INTO track_arrangement (track_id, arrangement_id, source, suggestion_id) "
        "VALUES (?, ?, ?, ?)",
        id, target, source, sugg);
  }
}

// After an estimate moved tracks away: an estimated (source 1) arrangement with no track
// left is removed, and so is its estimated composition once it has no arrangement, no
// relation and nobody's preferred version. Otherwise an accepted suggestion that moves
// songs on (e.g. MusicBrainz after a title match) leaves an empty shell behind.
void prune_left(db::Connection& w, const std::set<std::string>& left) {
  std::set<std::string> comps;
  for (const auto& arr : left) {
    auto comp = w.scalar_text(
        "SELECT composition_id FROM arrangement WHERE id = ?1 AND source = 1 "
        "AND NOT EXISTS (SELECT 1 FROM track_arrangement WHERE arrangement_id = ?1)",
        arr);
    if (!comp) continue;
    w.run("DELETE FROM participant WHERE entity_type = 'arrangement' AND entity_id = ?", arr);
    w.run("DELETE FROM arrangement WHERE id = ?", arr);
    comps.insert(*comp);
  }
  for (const auto& comp : comps) {
    if (!w.scalar_int(
            "SELECT 1 FROM composition c WHERE c.id = ?1 AND c.source = 1 "
            "AND NOT EXISTS (SELECT 1 FROM arrangement WHERE composition_id = ?1) "
            "AND NOT EXISTS (SELECT 1 FROM relation WHERE (from_type = 'composition' AND from_id = ?1) "
            "  OR (to_type = 'composition' AND to_id = ?1)) "
            "AND NOT EXISTS (SELECT 1 FROM preferred_version WHERE composition_id = ?1)",
            comp))
      continue;
    w.run("DELETE FROM participant WHERE entity_type = 'composition' AND entity_id = ?", comp);
    w.run("DELETE FROM field_override WHERE entity_type = 'composition' AND entity_id = ?", comp);
    w.run("DELETE FROM composition WHERE id = ?", comp);
  }
}

}  // namespace

json suggestion_json(db::Connection& db, const std::string& id) {
  auto st = db.prepare(
      "SELECT id, rule, method, model, score, status, created_at, decided_at, payload, evidence FROM suggestion "
      "WHERE id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Suggestion not found");
  auto payload = njson::parse(st.text(8));
  json j = {{"id", st.text(0)},
            {"rule", st.text(1)},
            {"method", st.text(2)},
            {"model", st.opt_text(3) ? json(*st.opt_text(3)) : json(nullptr)},
            {"score", st.real(4)},
            {"status", st.text(5)},
            {"createdAt", iso_time(st.int64(6))},
            {"decidedAt", st.opt_int64(7) ? json(iso_time(*st.opt_int64(7))) : json(nullptr)}};
  try {
    j["summary"] = summary(db, payload);
    j["targets"] = targets(db, payload);
  } catch (const std::exception&) {
    j["summary"] = payload.value("action", "");
    j["targets"] = json::array();
  }
  j["proposal"] = payload;
  j["evidence"] = st.opt_text(9) ? json::parse(*st.opt_text(9), nullptr, false) : json(nullptr);
  return j;
}

void validate_proposal(db::Connection& db, const njson& p) {
  if (!p.is_object()) throw bad_request("proposal must be an object");
  auto action = str(p, "action");
  if (action == "link_composition") {
    if (p.contains("compositionId") && p["compositionId"].is_string()) {
      if (!exists(db, "composition", p["compositionId"])) throw not_found("Composition not found");
    } else if (normalize(str(p, "name")).empty()) {
      throw bad_request("proposal.name is empty");
    }
    for (const auto& t : str_list(p, "tracks")) require_track(db, t);
  } else if (action == "merge_arrangements") {
    if (!exists(db, "arrangement", str(p, "arrangementId"))) throw not_found("Arrangement not found");
    for (const auto& t : str_list(p, "tracks")) require_track(db, t);
  } else if (action == "relation") {
    if (!kRelationTypes.count(str(p, "type"))) throw bad_request("unknown relation type");
    check_ref(db, p.value("from", njson()), "from");
    check_ref(db, p.value("to", njson()), "to");
  } else if (action == "medley") {
    check_ref(db, p.value("from", njson()), "from");
    if (!p.contains("parts") || !p["parts"].is_array() || p["parts"].empty())
      throw bad_request("proposal.parts must be a non-empty array");
    for (const auto& part : p["parts"]) check_ref(db, part.value("to", njson()), "parts[].to");
  } else if (action == "link_title") {
    if (p.contains("titleId") && p["titleId"].is_string()) {
      if (!exists(db, "title", p["titleId"])) throw not_found("Title not found");
    } else if (normalize(str(p, "titleName")).empty()) {
      throw bad_request("proposal.titleName is empty");
    }
    for (const auto& t : str_list(p, "tracks")) require_track(db, t);
  } else if (action == "virtual_album") {
    if (normalize(str(p, "name")).empty()) throw bad_request("proposal.name is empty");
    for (const auto& t : str_list(p, "tracks")) require_track(db, t);
  } else if (action == "link_arrangement") {
    if (!p.contains("composition") || !p["composition"].is_object())
      throw bad_request("proposal.composition must be an object");
    const auto& comp = p["composition"];
    if (comp.contains("id") && comp["id"].is_string()) {
      if (!exists(db, "composition", comp["id"])) throw not_found("Composition not found");
    } else if (normalize(str(comp, "name")).empty()) {
      throw bad_request("proposal.composition.name is empty");
    }
    if (p.contains("arrangement") && !p["arrangement"].is_object() && !p["arrangement"].is_null())
      throw bad_request("proposal.arrangement must be an object or null");
    for (const auto& t : str_list(p, "tracks")) require_track(db, t);
    if (p.contains("originalTracks"))
      for (const auto& t : str_list(p, "originalTracks")) require_track(db, t);
  } else {
    throw bad_request("unknown proposal action: " + action);
  }
}

void apply_proposal(db::Connection& w, const njson& p, int source, const std::string& sid) {
  validate_proposal(w, p);
  const auto action = p["action"].get<std::string>();
  const auto now = db::now_ms();
  const auto sugg = opt(sid);
  std::set<std::string> left;  // arrangements tracks were moved out of
  if (action == "merge_arrangements") {
    const std::string arrangement = p["arrangementId"];
    link_tracks(w, p["tracks"], [&](const std::string&) { return arrangement; }, source, sugg, left);
  } else if (action == "link_composition") {
    // Groups songs that had no composition when the suggestion was made (same title and
    // composer). A song linked since then, say by MusicBrainz, keeps that link: a title
    // match is weaker evidence. With no song left there is nothing to do.
    njson tracks = njson::array();
    for (const auto& t : p["tracks"])
      if (!w.scalar_int("SELECT 1 FROM track_arrangement_eff WHERE track_id = ?", t.get<std::string>()))
        tracks.push_back(t);
    if (tracks.empty()) return;
    std::string comp = p.contains("compositionId") && p["compositionId"].is_string() ? p["compositionId"].get<std::string>()
                                                                                    : std::string();
    if (comp.empty()) {
      comp = ids::random_id();
      auto name = p["name"].get<std::string>();
      w.run(
          "INSERT INTO composition (id, name, name_norm, source, suggestion_id, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)",
          comp, name, normalize(name), source, sugg, now, now);
      std::vector<scanner::Person> composers;
      if (p.contains("composers"))
        for (const auto& n : p["composers"]) composers.push_back({n.get<std::string>(), "", "", ""});
      int pos = 0;
      for (const auto& person : composers) {
        scanner::upsert_artist(w, person);
        w.run(
            "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, "
            "source, suggestion_id) VALUES ('composition', ?, ?, 'composer', '', ?, ?, ?)",
            comp, scanner::artist_id(person.name), pos++, source, sugg);
      }
    }
    ArrangementPicker pick(w, comp, source, sugg, now);
    link_tracks(w, tracks, [&](const std::string& t) { return pick.for_track(t); }, source, sugg, left);
  } else if (action == "relation") {
    insert_relation(w, p["from"], p["to"], p["type"], p.value("subtype", njson()), p.value("position", njson()),
                    p.value("note", njson()), source, sid);
  } else if (action == "medley") {
    for (const auto& part : p["parts"])
      insert_relation(w, p["from"], part["to"], "medley", njson(), part.value("position", njson()), njson(), source,
                      sid);
  } else if (action == "link_title") {
    std::string title = p.contains("titleId") && p["titleId"].is_string() ? p["titleId"].get<std::string>() : "";
    if (title.empty()) {
      auto name = p["titleName"].get<std::string>();
      title = w.scalar_text("SELECT id FROM title WHERE name_norm = ?", normalize(name)).value_or("");
      if (title.empty()) {
        title = ids::random_id();
        w.run(
            "INSERT INTO title (id, name, name_norm, source, suggestion_id, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            title, name, normalize(name), source, sugg, now, now);
      }
    }
    for (const auto& t : p["tracks"])
      w.run("INSERT OR IGNORE INTO track_title (track_id, title_id, source, suggestion_id) VALUES (?, ?, ?, ?)",
            t.get<std::string>(), title, source, sugg);
  } else if (action == "link_arrangement") {
    // Composition: by id, by MusicBrainz work id, by name, or new.
    const auto& cj = p["composition"];
    std::string comp = cj.contains("id") && cj["id"].is_string() ? cj["id"].get<std::string>() : "";
    std::optional<std::string> mbid;
    if (cj.contains("musicBrainzWorkId") && cj["musicBrainzWorkId"].is_string()) mbid = cj["musicBrainzWorkId"].get<std::string>();
    if (comp.empty() && mbid) comp = w.scalar_text("SELECT id FROM composition WHERE mbz_work_id = ?", *mbid).value_or("");
    if (comp.empty())
      comp = w.scalar_text("SELECT id FROM composition WHERE name_norm = ? LIMIT 1", normalize(cj["name"].get<std::string>()))
                 .value_or("");
    if (comp.empty()) {
      comp = ids::random_id();
      auto name = cj["name"].get<std::string>();
      w.run(
          "INSERT INTO composition (id, name, name_norm, mbz_work_id, source, suggestion_id, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
          comp, name, normalize(name), mbid, source, sugg, now, now);
      int pos = 0;
      for (const auto& n : cj.value("composers", njson::array())) {
        scanner::Person person{n.get<std::string>(), "", "", ""};
        scanner::upsert_artist(w, person);
        w.run(
            "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source, "
            "suggestion_id) VALUES ('composition', ?, ?, 'composer', '', ?, ?, ?)",
            comp, scanner::artist_id(person.name), pos++, source, sugg);
      }
    } else if (mbid) {
      w.run("UPDATE composition SET mbz_work_id = COALESCE(mbz_work_id, ?) WHERE id = ?", *mbid, comp);
    }
    // originalTracks: the versions the others arrange, to the implicit original.
    if (p.contains("originalTracks")) {
      ArrangementPicker originals(w, comp, source, sugg, now);
      link_tracks(w, p["originalTracks"], [&](const std::string&) { return originals.implicit(); }, source, sugg, left);
    }
    // Arrangement: the implicit original when no name is given, else found or made by name.
    const njson aj = p.value("arrangement", njson());
    const bool named = aj.is_object() && aj.contains("name") && aj["name"].is_string() &&
                       !normalize(aj["name"].get<std::string>()).empty();
    if (!named) {
      // No arrangement named: each track to the original, or to its arrangers' arrangement.
      ArrangementPicker pick(w, comp, source, sugg, now);
      link_tracks(w, p["tracks"], [&](const std::string& t) { return pick.for_track(t); }, source, sugg, left);
    } else {
      std::string arrangement;
      auto name = aj["name"].get<std::string>();
      arrangement = w.scalar_text(
                         "SELECT id FROM arrangement WHERE composition_id = ? AND is_implicit = 0 AND eos_norm(name) = ?",
                         comp, normalize(name))
                        .value_or("");
      if (arrangement.empty()) {
        arrangement = ids::random_id();
        w.run(
            "INSERT INTO arrangement (id, composition_id, name, kind, is_implicit, source, suggestion_id, created_at, "
            "updated_at) VALUES (?, ?, ?, ?, 0, ?, ?, ?, ?)",
            arrangement, comp, name, aj.value("kind", scanner::arrangement_kind(name)), source, sugg, now, now);
        int pos = 0;
        for (const auto& n : aj.value("arrangers", njson::array())) {
          scanner::Person person{n.get<std::string>(), "", "", ""};
          scanner::upsert_artist(w, person);
          w.run(
              "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source, "
              "suggestion_id) VALUES ('arrangement', ?, ?, 'arranger', '', ?, ?, ?)",
              arrangement, scanner::artist_id(person.name), pos++, source, sugg);
        }
      }
      link_tracks(w, p["tracks"], [&](const std::string&) { return arrangement; }, source, sugg, left);
    }
  } else if (action == "virtual_album") {
    auto name = p["name"].get<std::string>();
    auto album = w.scalar_text("SELECT id FROM album WHERE kind = 'virtual' AND eos_norm(name) = ?", normalize(name))
                     .value_or("");
    if (album.empty()) {
      album = ids::random_id();
      auto first = p["tracks"][0].get<std::string>();
      w.run(
          "INSERT INTO album (id, pid, library_id, name, display_artist, kind, source, created_at, updated_at) "
          "SELECT ?1, ?1, t.library_id, ?2, al.display_artist, 'virtual', ?3, ?4, ?4 FROM track t "
          "JOIN album al ON al.id = t.album_id WHERE t.id = ?5",
          album, name, source, now, first);
    }
    int64_t n = 0;
    for (const auto& t : p["tracks"])
      w.run(
          "INSERT OR REPLACE INTO album_track (album_id, track_id, track_number, source, suggestion_id) "
          "VALUES (?, ?, ?, ?, ?)",
          album, t.get<std::string>(), ++n, source, sugg);
    scanner::finish_custom(w, now);
  }
  prune_left(w, left);
}

}  // namespace eos::api::ext
