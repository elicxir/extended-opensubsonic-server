#include "suggest/rules.h"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <unordered_map>

#include "core/crypto.h"
#include "core/ids.h"
#include "core/log.h"
#include "core/normalize.h"
#include "core/strings.h"

namespace eos::suggest {
namespace {

using json = nlohmann::json;

std::vector<std::regex> compile(const std::vector<std::string>& patterns) {
  std::vector<std::regex> out;
  for (const auto& p : patterns) {
    try {
      out.emplace_back(p, std::regex::icase | std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
      log::warn("suggest: bad derivative pattern '{}': {}", p, e.what());
    }
  }
  return out;
}

// Trailing bracketed notes: "(Piano Ver.)", "[Remastered]", "～arrange～", "【...】".
const std::regex& trailing_brackets() {
  static const std::regex re(
      R"(\s*(\([^()]*\)|\[[^\[\]]*\]|（[^（）]*）|【[^【】]*】|～[^～]*～|〜[^〜]*〜|-[^-]*-)\s*$)");
  return re;
}

std::string strip(std::string title, const std::vector<std::regex>& patterns) {
  for (const auto& re : patterns) title = std::regex_replace(title, re, " ");
  for (int i = 0; i < 4; ++i) {
    auto next = std::regex_replace(title, trailing_brackets(), "");
    if (next == title || trim(next).empty()) break;
    title = next;
  }
  return trim(title);
}

// same_composition groups whose songs are all this short are jingles ("Game Over",
// "Stage Clear"): sharing a name and a composer says little about being one piece.
constexpr int64_t kJingleMs = 30'000;
constexpr double kJingleScore = 0.4;

struct Track {
  std::string id, title, album_id, base, arrangement, composition;
  int64_t duration_ms = 0;
  std::set<std::string> composers, arrangers, performers, titles;
  std::map<std::string, std::string> composer_names;  // normalized -> as the tags write it
  std::optional<std::string> derivative;
};

std::string fp(const std::string& key) {
  auto d = crypto::sha256(key);
  return crypto::hex(d.data(), 16);
}

std::string set_key(const std::set<std::string>& s) {
  return join(std::vector<std::string>(s.begin(), s.end()), ",");
}

// Compiling std::regex is slow; keep the compiled patterns per thread and config.
const std::vector<std::regex>& patterns_for(const Config& cfg) {
  static thread_local std::vector<std::string> source;
  static thread_local std::vector<std::regex> compiled;
  static thread_local bool ready = false;
  if (!ready || source != cfg.derivative_patterns) {
    compiled = compile(cfg.derivative_patterns);
    source = cfg.derivative_patterns;
    ready = true;
  }
  return compiled;
}

}  // namespace

std::string base_title(const std::string& title, const Config& cfg) {
  return normalize(strip(title, patterns_for(cfg)));
}

std::optional<std::string> derivative_subtype(const std::string& title, const Config& cfg) {
  for (const auto& re : patterns_for(cfg)) {
    std::smatch m;
    if (!std::regex_search(title, m, re)) continue;
    auto n = normalize(m.str());
    if (n.find("tvsize") != std::string::npos || n.find("tvver") != std::string::npos ||
        n.find("tvedit") != std::string::npos)
      return "tv_size";
    if (n.find("offvocal") != std::string::npos || n.find("karaoke") != std::string::npos) return "off_vocal";
    if (n.find("instrumental") != std::string::npos) return "instrumental";
    if (n.find("remix") != std::string::npos || n.find("rmx") != std::string::npos) return "remix";
    return "other";
  }
  return std::nullopt;
}

int64_t run_rules(db::Connection& c, const Config& cfg) {
  const auto& patterns = patterns_for(cfg);
  std::vector<Track> tracks;
  std::unordered_map<std::string, size_t> by_id;
  {
    auto st = c.prepare(
        "SELECT t.id, t.title, t.album_id, COALESCE(e.arrangement_id, ''), COALESCE(a.composition_id, ''), "
        "t.duration_ms FROM track t LEFT JOIN track_arrangement_eff e ON e.track_id = t.id "
        "LEFT JOIN arrangement a ON a.id = e.arrangement_id WHERE t.missing = 0 ORDER BY t.id");
    while (st.step()) {
      Track t{st.text(0), st.text(1), st.text(2), {}, st.text(3), st.text(4), st.int64(5), {}, {}, {}, {}, {}};
      t.base = normalize(strip(t.title, patterns));
      t.derivative = derivative_subtype(t.title, cfg);
      by_id[t.id] = tracks.size();
      tracks.push_back(std::move(t));
    }
  }
  {
    // Effective people per track and role (highest source wins).
    auto st = c.prepare(
        "SELECT p.entity_id, p.role, a.name FROM participant p JOIN artist a ON a.id = p.artist_id "
        "WHERE p.entity_type = 'track' AND p.role IN ('composer', 'arranger', 'performer') AND p.source = ("
        "  SELECT MAX(source) FROM participant q WHERE q.entity_type = 'track' "
        "  AND q.entity_id = p.entity_id AND q.role = p.role)");
    while (st.step()) {
      auto it = by_id.find(st.text(0));
      if (it == by_id.end()) continue;
      auto& t = tracks[it->second];
      auto role = st.text(1);
      auto name = normalize(st.text(2));
      if (role == "composer") {
        t.composers.insert(name);
        t.composer_names.emplace(name, st.text(2));
      }
      else if (role == "arranger") t.arrangers.insert(name);
      else t.performers.insert(name);
    }
  }
  {
    auto st = c.prepare("SELECT track_id, title_id FROM track_title WHERE excluded = 0");
    while (st.step())
      if (auto it = by_id.find(st.text(0)); it != by_id.end()) tracks[it->second].titles.insert(st.text(1));
  }

  struct Candidate {
    std::string rule, fingerprint;
    json payload;
    double score = 1.0;
    std::optional<std::string> evidence;
  };
  std::vector<Candidate> found;

  // -- same_composition
  std::map<std::string, std::vector<const Track*>> groups;
  for (const auto& t : tracks)
    if (t.composition.empty() && !t.base.empty() && !t.composers.empty())
      groups[t.base + "|" + set_key(t.composers)].push_back(&t);
  for (const auto& [key, members] : groups) {
    auto existing = c.scalar_text(
        "SELECT co.id FROM composition co WHERE co.name_norm = ? AND ("
        "  SELECT group_concat(n, ',') FROM (SELECT DISTINCT eos_norm(a.name) AS n FROM participant p "
        "  JOIN artist a ON a.id = p.artist_id WHERE p.entity_type = 'composition' AND p.entity_id = co.id "
        "  AND p.role = 'composer' ORDER BY n)) = ? LIMIT 1",
        members.front()->base, set_key(members.front()->composers));
    if (members.size() < 2 && !existing) continue;
    json ids = json::array();
    for (auto* m : members) ids.push_back(m->id);
    // The names as written: accepting the suggestion must not rename anybody to the
    // normalized form the groups are keyed by.
    std::vector<std::string> composers;
    for (const auto& n : members.front()->composers) composers.push_back(members.front()->composer_names.at(n));
    json payload = {{"action", "link_composition"},
                    {"name", strip(members.front()->title, patterns)},
                    {"composers", composers},
                    {"tracks", ids}};
    if (existing) payload["compositionId"] = *existing;
    Candidate cand{"same_composition", fp("same_composition|" + key), payload};
    if (std::all_of(members.begin(), members.end(), [](auto* m) { return m->duration_ms <= kJingleMs; })) {
      int64_t longest = 0;
      for (auto* m : members) longest = std::max(longest, m->duration_ms);
      cand.score = kJingleScore;
      cand.evidence = json{{"reason", "jingle_length"},
                           {"longestMs", longest},
                           {"note", "Every song is 30 s or shorter. Short cues of different games often share a name "
                                    "and a composer credit; listen before accepting."}}
                          .dump();
    }
    found.push_back(std::move(cand));
  }

  // -- performance_variant
  std::map<std::string, std::vector<const Track*>> variants;
  for (const auto& t : tracks)
    if (!t.composition.empty() && !t.arrangers.empty() && !t.base.empty())
      variants[t.composition + "|" + t.base + "|" + set_key(t.arrangers)].push_back(&t);
  for (const auto& [key, members] : variants) {
    std::map<std::string, int> per_arrangement;
    for (auto* m : members) ++per_arrangement[m->arrangement];
    if (per_arrangement.size() < 2) continue;
    auto target = std::max_element(per_arrangement.begin(), per_arrangement.end(),
                                   [](const auto& a, const auto& b) { return a.second < b.second; })
                      ->first;
    json ids = json::array();
    for (auto* m : members)
      if (m->arrangement != target) ids.push_back(m->id);
    found.push_back({"performance_variant", fp("performance_variant|" + key),
                     {{"action", "merge_arrangements"},
                      {"compositionId", members.front()->composition},
                      {"arrangementId", target},
                      {"tracks", ids}}});
  }

  // -- derivative
  std::unordered_map<std::string, std::vector<const Track*>> by_base;
  for (const auto& t : tracks)
    if (!t.base.empty()) by_base[t.base].push_back(&t);
  auto relation_exists = [&](const std::string& from, const std::string& to, const char* type) {
    return c.scalar_int(
               "SELECT 1 FROM relation WHERE ((from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)) "
               "AND type = ?3",
               from, to, type)
        .has_value();
  };
  for (const auto& t : tracks) {
    if (!t.derivative || t.base.empty()) continue;
    std::vector<const Track*> same_album, same_work;
    for (auto* o : by_base[t.base]) {
      if (o->id == t.id || o->derivative) continue;
      if (o->album_id == t.album_id) same_album.push_back(o);
      else if (std::any_of(o->titles.begin(), o->titles.end(), [&](const auto& w) { return t.titles.count(w); }))
        same_work.push_back(o);
    }
    auto pick = [&](const std::vector<const Track*>& pool) -> const Track* {
      if (pool.size() == 1) return pool.front();
      // Several candidates (e.g. "Theme" and "Theme (Piano Ver.)"): the original is
      // the one whose title is the bare base title.
      const Track* bare = nullptr;
      for (auto* o : pool) {
        if (normalize(o->title) != t.base) continue;
        if (bare) return nullptr;
        bare = o;
      }
      return bare;
    };
    const auto* original = pick(!same_album.empty() ? same_album : same_work);
    if (!original) continue;  // none or ambiguous
    if (relation_exists(t.id, original->id, "derivative")) continue;
    found.push_back({"derivative", fp("derivative|" + t.id + "|" + original->id),
                     {{"action", "relation"},
                      {"type", "derivative"},
                      {"subtype", *t.derivative},
                      {"from", {{"type", "track"}, {"id", t.id}}},
                      {"to", {{"type", "track"}, {"id", original->id}}}}});
  }

  // -- medley
  // Japanese titles use 〜 and " - " for subtitles ("Theme ～Prologue～"), so a title is
  // only a medley when it says so (メドレー / medley) or joins three or more titles.
  for (const auto& t : tracks) {
    const auto whole = normalize(t.title);
    const bool says_medley =
        whole.find("medley") != std::string::npos || whole.find("めどれー") != std::string::npos;
    auto parts = split_any(strip(t.title, patterns), cfg.medley_separators);
    if (parts.size() < 2) continue;
    json matched = json::array();
    std::set<std::string> targets;
    int position = 0;
    for (const auto& part : parts) {
      ++position;
      auto base = normalize(strip(part, patterns));
      if (base.empty() || base == t.base) continue;
      std::optional<json> to;
      if (auto comp = c.scalar_text("SELECT id FROM composition WHERE name_norm = ? LIMIT 1", base)) {
        to = json{{"type", "composition"}, {"id", *comp}};
      } else if (auto it = by_base.find(base); it != by_base.end()) {
        for (auto* o : it->second)
          if (o->id != t.id) {
            to = json{{"type", "track"}, {"id", o->id}};
            break;
          }
      }
      if (!to || !targets.insert((*to)["id"].get<std::string>()).second) continue;  // no match / repeated
      matched.push_back({{"position", position}, {"to", *to}});
    }
    if (matched.size() < 2 || (!says_medley && matched.size() < 3)) continue;
    found.push_back({"medley", fp("medley|" + t.id),
                     {{"action", "medley"}, {"from", {{"type", "track"}, {"id", t.id}}}, {"parts", matched}}});
  }

  // -- store
  int64_t added = 0;
  const auto now = db::now_ms();
  std::set<std::string> live;
  for (const auto& f : found) {
    live.insert(f.fingerprint);
    auto status = c.scalar_text("SELECT status FROM suggestion WHERE fingerprint = ?", f.fingerprint);
    if (!status) {
      c.run(
          // Every built-in rule matches names (titles, composers, "(Remix)" and the like).
          "INSERT INTO suggestion (id, rule, method, origin, evidence, fingerprint, payload, score, status, created_at) "
          "VALUES (?, ?, 'rule', 'name', ?, ?, ?, ?, 'pending', ?)",
          ids::random_id(), f.rule, f.evidence, f.fingerprint, f.payload.dump(), f.score, now);
      ++added;
    } else if (*status == "pending" || *status == "stale") {
      c.run("UPDATE suggestion SET payload = ?, score = ?, evidence = ?, status = 'pending' WHERE fingerprint = ?",
            f.payload.dump(), f.score, f.evidence, f.fingerprint);
    }
  }
  // Pending rule suggestions that were not produced again no longer apply.
  std::vector<std::string> stale;
  {
    auto st = c.prepare("SELECT fingerprint FROM suggestion WHERE method = 'rule' AND status = 'pending'");
    while (st.step())
      if (!live.count(st.text(0))) stale.push_back(st.text(0));
  }
  for (const auto& f : stale) c.run("UPDATE suggestion SET status = 'stale' WHERE fingerprint = ?", f);
  return added;
}

}  // namespace eos::suggest
