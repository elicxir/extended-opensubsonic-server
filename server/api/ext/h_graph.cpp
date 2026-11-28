// GET /ext/graph: the neighbourhood of one item as nodes and edges, for drawing related
// songs as a graph.
//
// Groups are nodes, not cliques: the songs of a composition all point at the composition
// node instead of at each other. Only `relation` rows join two songs directly, and, with
// include=similar, the computed similarity (docs/similarity.md), which is followed from
// the seed only.
#include <algorithm>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "api/ext/model.h"
#include "api/subsonic/builders.h"
#include "core/strings.h"

namespace eos::api::ext {
namespace {

// Which kinds of edges to follow (?include=).
struct Include {
  bool composition = false, relation = false, title = false, artist = false, album = false, similar = false;
};

// ?similarKind= &similarK= &similarMinScore= &similarOtherAlbums=
struct Similar {
  std::string kind = "audio_embed";
  int64_t k = 5;
  double min_score = -1e300;
  bool other_albums = true;  // leave out songs of the same (physical) album
};

struct Edge {
  std::string key;  // the same edge found from either end has the same key
  std::string from, to;
  json j;
  bool seed_only = false;  // walked from the seed only (similar): every song has k of them
};

std::string node_id(std::string_view type, const std::string& id) {
  return std::string(type == "track" ? "song" : type) + ":" + id;
}

std::pair<std::string, std::string> split_node(const std::string& node) {
  auto p = node.find(':');
  return {node.substr(0, p), node.substr(p + 1)};
}

// Columns from `col` on: the suggestion's score and rule, then the row's origin and
// origin_ref (each NULL when none).
json prov_fields(int64_t source, const std::optional<std::string>& method, const db::Statement& st, int col) {
  json j = {{"provenance", provenance(source, method)}};
  if (source == 1 && !st.is_null(col)) j["confidence"] = st.real(col);
  if (source == 1 && !st.is_null(col + 1)) j["rule"] = st.text(col + 1);
  if (auto o = origin_of(source, st.opt_text(col + 2))) j["origin"] = *o;
  if (source == 1 && !st.is_null(col + 3)) j["originRef"] = st.text(col + 3);
  return j;
}

// ?origin=: keep only the links that rest on one of these (external, audio, name, llm,
// tag, manual). Computed links (similar) are always kept.
using Origins = std::optional<std::set<std::string>>;

class Graph {
 public:
  Graph(ExtCtx& c, Include inc, std::set<std::string> roles, Similar sim, Origins origins)
      : c_(c), db_(c.db), inc_(inc), roles_(std::move(roles)), sim_(std::move(sim)), origins_(std::move(origins)),
        min_(c.min_source()) {}

  // Every edge touching `node` (cached). Strong kinds first, so that they win the
  // node budget.
  const std::vector<Edge>& edges_of(const std::string& node) {
    auto it = cache_.find(node);
    if (it != cache_.end()) return it->second;
    auto [type, id] = split_node(node);
    std::vector<Edge> out;
    if (type == "song") {
      if (inc_.composition) performances(out, "ta.track_id = ?1", id);
      if (inc_.relation) relations(out, "track", id);
      if (inc_.title) titles(out, "tt.track_id = ?1", id);
      if (inc_.artist) credits(out, "p.entity_type = 'track' AND p.entity_id = ?1", id);
      if (inc_.album) albums_of(out, "t.id = ?1", "at.track_id = ?1", id);
      if (inc_.similar) similar(out, id);
    } else if (type == "composition") {
      if (inc_.composition) performances(out, "a.composition_id = ?1", id);
      if (inc_.relation) relations(out, "composition", id);
      if (inc_.artist) credits(out, "p.entity_type = 'composition' AND p.entity_id = ?1", id);
    } else if (type == "title") {
      if (inc_.title) titles(out, "tt.title_id = ?1", id);
    } else if (type == "artist") {
      if (inc_.artist) credits(out, "p.artist_id = ?1", id);
    } else if (type == "album") {
      if (inc_.album) albums_of(out, "t.album_id = ?1", "at.album_id = ?1", id);
    }
    return cache_.emplace(node, std::move(out)).first->second;
  }

 private:
  void add(std::vector<Edge>& out, std::string key, std::string from, std::string to, json j, bool seed_only = false) {
    if (origins_ && j.value("provenance", std::string()) != "computed" &&
        !origins_->count(j.value("origin", std::string())))
      return;
    json e = {{"from", from}, {"to", to}};
    e.update(j);
    out.push_back({std::move(key), std::move(from), std::move(to), std::move(e), seed_only});
  }

  // song -- song: the song's k most similar songs of one kind, best first. The table holds
  // each song's top K as unordered pairs with symmetric scores, so the rows touching the
  // song, ordered by score, are its own top list (docs/similarity.md).
  void similar(std::vector<Edge>& out, const std::string& id) {
    auto st = db_.prepare(
        "SELECT s.other, s.score FROM ("
        "  SELECT track_b AS other, score FROM similarity WHERE track_a = ?1 AND kind = ?2 "
        "  UNION ALL SELECT track_a, score FROM similarity WHERE track_b = ?1 AND kind = ?2) s "
        "JOIN track t ON t.id = s.other AND t.missing = 0 "
        "WHERE s.score >= ?3 AND (?4 = 0 OR t.album_id <> (SELECT album_id FROM track WHERE id = ?1)) "
        "ORDER BY s.score DESC, s.other LIMIT ?5");
    st.bind_all(id, sim_.kind, sim_.min_score, sim_.other_albums ? 1 : 0, sim_.k);
    while (st.step()) {
      auto other = st.text(0);
      auto from = node_id("song", id), to = node_id("song", other);
      auto key = "similar|" + sim_.kind + "|" + std::min(id, other) + "|" + std::max(id, other);
      add(out, std::move(key), from, to,
          {{"type", "similar"}, {"kind", sim_.kind}, {"score", st.real(1)}, {"provenance", "computed"}}, true);
    }
  }

  // song -> composition, through the song's effective arrangement.
  void performances(std::vector<Edge>& out, const char* where, const std::string& id) {
    auto st = db_.prepare(std::string(
        "SELECT ta.track_id, a.composition_id, a.id, a.name, a.kind, a.is_implicit, ta.source, s.method, s.score, s.rule, "
        "ta.origin, ta.origin_ref "
        "FROM track_arrangement ta JOIN arrangement a ON a.id = ta.arrangement_id "
        "JOIN track t ON t.id = ta.track_id AND t.missing = 0 "
        "JOIN album al ON al.id = t.album_id "
        "LEFT JOIN suggestion s ON s.id = ta.suggestion_id "
        "WHERE ta.source >= ?2 AND ta.source = (SELECT MAX(source) FROM track_arrangement x "
        "  WHERE x.track_id = ta.track_id AND x.source >= ?2) AND ") + where +
        " ORDER BY a.is_implicit DESC, COALESCE(a.name, ''), COALESCE(al.release_date, ''), al.name, "
        "COALESCE(t.disc_number, 0), COALESCE(t.track_number, 0)");
    st.bind_all(id, min_);
    while (st.step()) {
      auto from = node_id("song", st.text(0)), to = node_id("composition", st.text(1));
      json j = {{"type", "performance"},
                {"arrangementId", st.text(2)},
                {"arrangementName", st.opt_text(3) ? json(*st.opt_text(3)) : json(nullptr)},
                {"arrangementKind", st.text(4)},
                {"isOriginal", st.int64(5) != 0}};
      j.update(prov_fields(st.int64(6), st.opt_text(7), st, 8));
      add(out, "performance|" + from + "|" + to, from, to, std::move(j));
    }
  }

  // relation rows (medley, quote, derivative, cover, related); directed from -> to.
  void relations(std::vector<Edge>& out, const char* type, const std::string& id) {
    static const char* alive =
        "(CASE r.{0}_type WHEN 'track' THEN EXISTS (SELECT 1 FROM track t WHERE t.id = r.{0}_id AND t.missing = 0) "
        "ELSE EXISTS (SELECT 1 FROM composition co WHERE co.id = r.{0}_id) END)";
    auto side = [&](const char* s) {
      std::string x = alive;
      for (auto p = x.find("{0}"); p != std::string::npos; p = x.find("{0}")) x.replace(p, 3, s);
      return x;
    };
    auto st = db_.prepare(
        "SELECT r.id, r.from_type, r.from_id, r.to_type, r.to_id, r.type, r.subtype, r.position, r.note, "
        "r.source, s.method, s.score, s.rule, r.origin, r.origin_ref FROM relation r LEFT JOIN suggestion s ON s.id = r.suggestion_id "
        "WHERE r.source >= ?3 AND ((r.from_type = ?1 AND r.from_id = ?2) OR (r.to_type = ?1 AND r.to_id = ?2)) "
        "AND " + side("from") + " AND " + side("to") + " ORDER BY r.type, COALESCE(r.position, 0), r.id");
    st.bind_all(type, id, min_);
    while (st.step()) {
      auto from = node_id(st.text(1), st.text(2)), to = node_id(st.text(3), st.text(4));
      json j = {{"type", st.text(5)},
                {"directed", true},
                {"relationId", st.text(0)},
                {"subtype", st.opt_text(6) ? json(*st.opt_text(6)) : json(nullptr)},
                {"position", st.opt_int64(7) ? json(*st.opt_int64(7)) : json(nullptr)},
                {"note", st.opt_text(8) ? json(*st.opt_text(8)) : json(nullptr)}};
      j.update(prov_fields(st.int64(9), st.opt_text(10), st, 11));
      add(out, "relation|" + st.text(0), from, to, std::move(j));
    }
  }

  // song -> title (work). Manual exclusions applied; the strongest row gives the provenance.
  void titles(std::vector<Edge>& out, const char* where, const std::string& id) {
    auto st = db_.prepare(std::string(
        "SELECT tt.track_id, tt.title_id, MAX(tt.source), s.method, s.score, s.rule, tt.origin, tt.origin_ref "
        "FROM track_title tt "
        "JOIN track t ON t.id = tt.track_id AND t.missing = 0 JOIN album al ON al.id = t.album_id "
        "LEFT JOIN suggestion s ON s.id = tt.suggestion_id "
        "WHERE tt.excluded = 0 AND tt.source >= ?2 AND NOT EXISTS (SELECT 1 FROM track_title x "
        "  WHERE x.track_id = tt.track_id AND x.title_id = tt.title_id AND x.excluded = 1) AND ") + where +
        " GROUP BY tt.track_id, tt.title_id ORDER BY COALESCE(al.release_date, ''), al.name, "
        "COALESCE(t.disc_number, 0), COALESCE(t.track_number, 0), t.path, tt.title_id");
    st.bind_all(id, min_);
    while (st.step()) {
      auto from = node_id("song", st.text(0)), to = node_id("title", st.text(1));
      json j = {{"type", "title"}};
      j.update(prov_fields(st.int64(2), st.opt_text(3), st, 4));
      add(out, "title|" + from + "|" + to, from, to, std::move(j));
    }
  }

  // song / composition -> artist, one edge per role.
  void credits(std::vector<Edge>& out, const char* where, const std::string& id) {
    auto st = db_.prepare(std::string(
        "SELECT p.entity_type, p.entity_id, p.artist_id, p.role, p.sub_role, p.source, s.method, s.score, s.rule, "
        "p.origin, p.origin_ref "
        "FROM participant p LEFT JOIN suggestion s ON s.id = p.suggestion_id "
        "WHERE p.entity_type IN ('track', 'composition') AND p.source >= ?2 AND p.source = ("
        "  SELECT MAX(source) FROM participant q WHERE q.entity_type = p.entity_type "
        "  AND q.entity_id = p.entity_id AND q.role = p.role AND q.source >= ?2) "
        "AND (p.entity_type <> 'track' OR EXISTS (SELECT 1 FROM track t WHERE t.id = p.entity_id AND t.missing = 0)) "
        "AND (p.entity_type <> 'composition' OR EXISTS (SELECT 1 FROM composition co WHERE co.id = p.entity_id)) "
        "AND ") + where + " ORDER BY p.entity_type, p.role, p.position, p.entity_id");
    st.bind_all(id, min_);
    while (st.step()) {
      auto role = st.text(3);
      if (!roles_.empty() && !roles_.count(role)) continue;
      auto from = node_id(st.text(0), st.text(1)), to = node_id("artist", st.text(2));
      auto sub = st.text(4);
      json j = {{"type", "credit"}, {"role", role}, {"subRole", sub.empty() ? json(nullptr) : json(sub)}};
      j.update(prov_fields(st.int64(5), st.opt_text(6), st, 7));
      add(out, "credit|" + from + "|" + to + "|" + role + "|" + sub, from, to, std::move(j));
    }
  }

  // song -> album: the physical album, and the virtual albums the song was put in.
  void albums_of(std::vector<Edge>& out, const char* where_physical, const char* where_virtual, const std::string& id) {
    {
      auto st = db_.prepare(std::string(
          "SELECT t.id, t.album_id FROM track t WHERE t.missing = 0 AND ") + where_physical +
          " ORDER BY COALESCE(t.disc_number, 0), COALESCE(t.track_number, 0), t.path");
      st.bind(1, id);
      while (st.step()) {
        auto from = node_id("song", st.text(0)), to = node_id("album", st.text(1));
        add(out, "album|" + from + "|" + to, from, to, {{"type", "album"}, {"virtual", false}, {"provenance", "tag"}, {"origin", "tag"}});
      }
    }
    auto st = db_.prepare(std::string(
        "SELECT at.track_id, at.album_id, MAX(at.source), s.method, s.score, s.rule, at.origin, at.origin_ref "
        "FROM album_track at "
        "JOIN track t ON t.id = at.track_id AND t.missing = 0 "
        "JOIN album al ON al.id = at.album_id AND al.missing = 0 "
        "LEFT JOIN suggestion s ON s.id = at.suggestion_id "
        "WHERE at.excluded = 0 AND at.source >= ?2 AND NOT EXISTS (SELECT 1 FROM album_track x "
        "  WHERE x.track_id = at.track_id AND x.album_id = at.album_id AND x.excluded = 1) AND ") + where_virtual +
        " GROUP BY at.track_id, at.album_id "
        "ORDER BY COALESCE(at.disc_number, 0), COALESCE(at.track_number, 0), at.track_id, at.album_id");
    st.bind_all(id, min_);
    while (st.step()) {
      auto from = node_id("song", st.text(0)), to = node_id("album", st.text(1));
      json j = {{"type", "album"}, {"virtual", true}};
      j.update(prov_fields(st.int64(2), st.opt_text(3), st, 4));
      add(out, "album|" + from + "|" + to, from, to, std::move(j));
    }
  }

  ExtCtx& c_;
  db::Connection& db_;
  Include inc_;
  std::set<std::string> roles_;
  Similar sim_;
  Origins origins_;
  int min_;
  std::unordered_map<std::string, std::vector<Edge>> cache_;
};

int64_t int_param(ExtCtx& c, std::string_view name, int64_t def, int64_t lo, int64_t hi) {
  auto v = c.query(name);
  if (!v) return def;
  try {
    size_t used = 0;
    int64_t n = std::stoll(*v, &used);
    if (used != v->size()) throw std::invalid_argument("trailing");
    return std::clamp(n, lo, hi);
  } catch (const std::exception&) {
    throw bad_request(std::string(name) + " must be an integer");
  }
}

Include parse_include(ExtCtx& c) {
  Include inc;
  for (const auto& raw : split_any(c.query("include").value_or("composition,relation,title"), {","})) {
    auto k = trim(raw);
    if (k == "composition") inc.composition = true;
    else if (k == "relation") inc.relation = true;
    else if (k == "title") inc.title = true;
    else if (k == "artist") inc.artist = true;
    else if (k == "album") inc.album = true;
    else if (k == "similar") inc.similar = true;
    else throw bad_request("include: unknown kind '" + k + "' (composition, relation, title, artist, album, similar)");
  }
  return inc;
}

// "song:<id>" (or "track:<id>") etc. -> the node id. 404 when the item does not exist.
std::string parse_seed(ExtCtx& c) {
  auto seed = c.query("seed").value_or("");
  auto p = seed.find(':');
  if (p == std::string::npos || p == 0 || p + 1 == seed.size())
    throw bad_request("seed is required, as <type>:<id> (song, composition, title, artist, album)");
  auto type = seed.substr(0, p), id = seed.substr(p + 1);
  if (type == "track") type = "song";
  if (type == "song") require_track(c.db, id);
  else if (type == "composition" || type == "title" || type == "artist") {
    if (!exists(c.db, type.c_str(), id)) throw not_found("No such " + type + ": " + id);
  } else if (type == "album") {
    if (!c.db.scalar_int("SELECT 1 FROM album WHERE id = ? AND missing = 0", id)) throw not_found("No such album: " + id);
  } else {
    throw bad_request("seed: unknown type '" + type + "' (song, composition, title, artist, album)");
  }
  return type + ":" + id;
}

Similar parse_similar(ExtCtx& c, bool wanted) {
  Similar s;
  if (auto k = c.query("similarKind")) s.kind = *k;
  s.k = int_param(c, "similarK", s.k, 1, 50);
  if (auto v = c.query("similarMinScore")) {
    try {
      size_t used = 0;
      s.min_score = std::stod(*v, &used);
      if (used != v->size()) throw std::invalid_argument("trailing");
    } catch (const std::exception&) {
      throw bad_request("similarMinScore must be a number");
    }
  }
  if (auto v = c.query("similarOtherAlbums")) s.other_albums = !(*v == "false" || *v == "0");
  if (wanted && !c.db.scalar_int("SELECT 1 FROM similarity_set WHERE kind = ?", s.kind))
    throw bad_request("similarKind: no similarity of kind '" + s.kind + "' (see GET /ext/similarity)");
  return s;
}

Origins parse_origins(ExtCtx& c) {
  auto v = c.query("origin");
  if (!v) return std::nullopt;
  std::set<std::string> out;
  for (const auto& r : split_any(*v, {","})) {
    auto t = trim(r);
    if (t.empty()) continue;
    if (!valid_origin(t) && t != "tag" && t != "manual")
      throw bad_request("origin: unknown '" + t + "' (external, audio, name, llm, tag, manual)");
    out.insert(t);
  }
  return out;
}

const std::string& other_end(const Edge& e, const std::string& node) { return e.from == node ? e.to : e.from; }

Reply graph(ExtCtx& c) {
  const auto seed = parse_seed(c);
  const auto depth = int_param(c, "depth", 2, 0, 4);
  const auto limit = static_cast<size_t>(int_param(c, "limit", 200, 1, 1000));
  const auto fanout = static_cast<size_t>(int_param(c, "fanout", 50, 1, 1000));
  std::set<std::string> roles;
  for (const auto& r : split_any(c.query("roles").value_or(""), {","})) roles.insert(trim(r));
  const auto inc = parse_include(c);
  Graph g(c, inc, std::move(roles), parse_similar(c, inc.similar), parse_origins(c));

  // Breadth first from the seed, so that when the node budget runs out the nearest
  // nodes are the ones kept. A node other than the seed with more than `fanout`
  // neighbours (a composer with hundreds of songs) is shown but not walked through.
  std::vector<std::string> order = {seed};
  std::unordered_map<std::string, int64_t> dist = {{seed, 0}};
  std::deque<std::string> queue = {seed};
  bool truncated = false;
  while (!queue.empty() && !truncated) {
    auto node = std::move(queue.front());
    queue.pop_front();
    const auto d = dist[node];
    if (d >= depth) continue;
    const auto& edges = g.edges_of(node);
    std::vector<std::string> next;
    std::unordered_set<std::string> seen;
    for (const auto& e : edges) {
      if (e.seed_only && node != seed) continue;
      if (const auto& o = other_end(e, node); o != node && seen.insert(o).second) next.push_back(o);
    }
    if (node != seed && next.size() > fanout) continue;
    for (auto& o : next) {
      if (dist.count(o)) continue;
      if (order.size() >= limit) {
        truncated = true;
        break;
      }
      dist[o] = d + 1;
      order.push_back(o);
      queue.push_back(std::move(o));
    }
  }

  // Every edge between two kept nodes, including those between nodes at the rim.
  json edges = json::array();
  std::unordered_set<std::string> edge_keys;
  std::unordered_map<std::string, std::pair<size_t, bool>> stats;  // degree, expanded
  for (const auto& node : order) {
    std::unordered_set<std::string> others;
    bool expanded = true;
    for (const auto& e : g.edges_of(node)) {
      const auto& o = other_end(e, node);
      others.insert(o);
      if (!dist.count(o)) expanded = false;
      else if (edge_keys.insert(e.key).second) edges.push_back(e.j);
    }
    stats[node] = {others.size(), expanded};
  }

  // Node bodies, loaded in batches per type.
  std::unordered_map<std::string, std::vector<std::string>> by_type;
  for (const auto& node : order) {
    auto [type, id] = split_node(node);
    by_type[type].push_back(id);
  }
  std::unordered_map<std::string, json> body;
  // The key is taken first: the right side of an assignment is evaluated before the left.
  for (auto& s : songs(c, by_type["song"])) {
    auto key = "song:" + s["id"].get<std::string>();
    body[key] = std::move(s);
  }
  for (auto& a : albums(c, by_type["album"])) {
    auto key = "album:" + a["id"].get<std::string>();
    body[key] = std::move(a);
  }
  for (const auto& id : by_type["composition"]) body["composition:" + id] = composition_json(c.db, id);
  for (const auto& id : by_type["title"]) body["title:" + id] = title_json(c.db, id);
  if (!by_type["artist"].empty()) {
    auto st = c.db.prepare("SELECT id, name FROM artist WHERE id IN (SELECT value FROM json_each(?))");
    st.bind(1, json_ids(by_type["artist"]));
    while (st.step()) body["artist:" + st.text(0)] = {{"id", st.text(0)}, {"name", st.text(1)}};
  }

  json nodes = json::array();
  for (const auto& node : order) {
    auto [type, id] = split_node(node);
    auto it = body.find(node);
    if (it == body.end()) continue;  // vanished between the queries
    const auto& b = it->second;
    const auto& label = b.contains("title") && type == "song" ? b["title"] : b["name"];
    nodes.push_back({{"id", node},
                     {"type", type},
                     {"label", label},
                     {"distance", dist[node]},
                     {"degree", stats[node].first},
                     {"expanded", stats[node].second},
                     {type, b}});
  }
  return ok({{"seed", seed}, {"depth", depth}, {"nodes", std::move(nodes)}, {"edges", std::move(edges)},
             {"truncated", truncated}});
}

}  // namespace

void add_graph_routes(ExtRouter& r) { r.add("GET", "/ext/graph", graph); }

}  // namespace eos::api::ext
