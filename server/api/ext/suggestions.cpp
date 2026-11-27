#include "api/ext/suggestions.h"

#include <format>
#include <set>

#include "api/ext/model.h"
#include "api/node.h"
#include "core/crypto.h"
#include "core/ids.h"
#include "core/normalize.h"
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
  if (p.contains("tracks"))
    for (const auto& t : p["tracks"]) add("track", t.get<std::string>());
  for (const char* k : {"from", "to"})
    if (p.contains(k) && p[k].is_object()) add(p[k].value("type", ""), p[k].value("id", ""));
  if (p.contains("parts"))
    for (const auto& part : p["parts"]) add(part["to"].value("type", ""), part["to"].value("id", ""));
  if (p.contains("compositionId") && p["compositionId"].is_string()) add("composition", p["compositionId"]);
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
  } else {
    throw bad_request("unknown proposal action: " + action);
  }
}

void apply_proposal(db::Connection& w, const njson& p, int source, const std::string& sid) {
  validate_proposal(w, p);
  const auto action = p["action"].get<std::string>();
  const auto now = db::now_ms();
  const auto sugg = opt(sid);
  if (action == "link_composition" || action == "merge_arrangements") {
    std::string arrangement;
    if (action == "merge_arrangements") {
      arrangement = p["arrangementId"];
    } else {
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
      arrangement = w.scalar_text("SELECT id FROM arrangement WHERE composition_id = ? AND is_implicit = 1", comp)
                        .value_or("");
      if (arrangement.empty()) {
        arrangement = ids::random_id();
        w.run(
            "INSERT INTO arrangement (id, composition_id, kind, is_implicit, source, suggestion_id, created_at, "
            "updated_at) VALUES (?, ?, 'original', 1, ?, ?, ?, ?)",
            arrangement, comp, source, sugg, now, now);
      }
    }
    for (const auto& t : p["tracks"])
      w.run(
          "INSERT OR REPLACE INTO track_arrangement (track_id, arrangement_id, source, suggestion_id) "
          "VALUES (?, ?, ?, ?)",
          t.get<std::string>(), arrangement, source, sugg);
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
}

}  // namespace eos::api::ext
