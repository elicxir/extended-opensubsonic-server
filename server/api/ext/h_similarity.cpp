// /ext/similarity: scores between songs computed by tools (audio-embed, ...), stored one
// kind at a time and replaced wholesale (docs/similarity.md). Not facts: no source, no
// suggestions, not exported.
#include <cmath>
#include <map>
#include <unordered_set>

#include "api/ext/model.h"

namespace eos::api::ext {
namespace {

using njson = nlohmann::json;

void check_kind(const std::string& kind) {
  bool good = !kind.empty() && kind.size() <= 64;
  for (char ch : kind) good = good && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_');
  if (!good) throw bad_request("kind must be 1-64 characters of a-z, 0-9 and _");
}

json set_json(const db::Statement& st) {
  json params;
  try {
    params = json::parse(st.text(2));
  } catch (const json::exception&) {
    params = nullptr;
  }
  return {{"kind", st.text(0)},
          {"model", st.text(1)},
          {"params", std::move(params)},
          {"trackCount", st.int64(3)},
          {"pairCount", st.int64(4)},
          {"createdAt", st.int64(5)}};
}

constexpr const char* kSetColumns = "SELECT kind, model, params, track_count, pair_count, created_at FROM similarity_set";

// [[a, b, score], ...] -> {(a, b): score} with a < b; a pair given twice keeps the higher score.
std::map<std::pair<std::string, std::string>, double> parse_pairs(const njson& b) {
  if (!b.contains("pairs") || !b["pairs"].is_array()) throw bad_request("pairs must be an array of [songId, songId, score]");
  std::map<std::pair<std::string, std::string>, double> out;
  size_t i = 0;
  for (const auto& p : b["pairs"]) {
    if (!p.is_array() || p.size() != 3 || !p[0].is_string() || !p[1].is_string() || !p[2].is_number())
      throw bad_request("pairs[" + std::to_string(i) + "] must be [songId, songId, score]");
    auto a = p[0].get<std::string>(), c = p[1].get<std::string>();
    double score = p[2].get<double>();
    if (!std::isfinite(score)) throw bad_request("pairs[" + std::to_string(i) + "]: score must be finite");
    ++i;
    if (a == c) continue;
    if (c < a) std::swap(a, c);
    auto [it, fresh] = out.try_emplace({std::move(a), std::move(c)}, score);
    if (!fresh) it->second = std::max(it->second, score);
  }
  return out;
}

Reply put_set(ExtCtx& c) {
  const auto& kind = c.args[0];
  check_kind(kind);
  const auto& b = c.body();
  if (!b.contains("model") || !b["model"].is_string() || b["model"].get<std::string>().empty())
    throw bad_request("model is required");
  if (b.contains("params") && !b["params"].is_object()) throw bad_request("params must be an object");
  if (b.contains("trackCount") && !b["trackCount"].is_number_integer()) throw bad_request("trackCount must be an integer");
  const auto model = b["model"].get<std::string>();
  const auto params = b.value("params", njson::object()).dump();
  const auto pairs = parse_pairs(b);

  auto [stored, skipped, tracks] = c.write([&](db::Connection& w) {
    std::unordered_set<std::string> known;
    auto ids = w.prepare("SELECT id FROM track");
    while (ids.step()) known.insert(ids.text(0));

    w.run("DELETE FROM similarity_set WHERE kind = ?", kind);  // the old pairs go with it
    w.run("INSERT INTO similarity_set (kind, model, params, track_count, pair_count, created_at) "
          "VALUES (?, ?, ?, 0, 0, ?)",
          kind, model, params, db::now_ms());
    auto ins = w.prepare("INSERT INTO similarity (kind, track_a, track_b, score) VALUES (?, ?, ?, ?)");
    int64_t n = 0, skip = 0;
    std::unordered_set<std::string> seen;
    for (const auto& [ab, score] : pairs) {
      if (!known.count(ab.first) || !known.count(ab.second)) {
        ++skip;
        continue;
      }
      ins.bind_all(kind, ab.first, ab.second, score);
      ins.run();
      ins.reset();
      seen.insert(ab.first);
      seen.insert(ab.second);
      ++n;
    }
    // trackCount is what the tool computed over; without it, the songs that have a pair.
    int64_t track_count = b.contains("trackCount") ? b["trackCount"].get<int64_t>() : static_cast<int64_t>(seen.size());
    w.run("UPDATE similarity_set SET track_count = ?, pair_count = ? WHERE kind = ?", track_count, n, kind);
    return std::tuple{n, skip, track_count};
  });
  return ok({{"kind", kind}, {"pairCount", stored}, {"skipped", skipped}, {"trackCount", tracks}});
}

}  // namespace

void add_similarity_routes(ExtRouter& r) {
  r.add(
      "GET", "/ext/similarity",
      [](ExtCtx& c) {
        json items = json::array();
        auto st = c.db.prepare(std::string(kSetColumns) + " ORDER BY kind");
        while (st.step()) items.push_back(set_json(st));
        auto n = static_cast<int64_t>(items.size());
        return page(std::move(items), n);
      },
      Access::admin);

  r.add("PUT", "/ext/similarity/{}", put_set, Access::admin);

  r.add(
      "DELETE", "/ext/similarity/{}",
      [](ExtCtx& c) {
        const auto& kind = c.args[0];
        bool gone = c.write([&](db::Connection& w) {
          w.run("DELETE FROM similarity_set WHERE kind = ?", kind);
          return w.changes() > 0;
        });
        if (!gone) throw not_found("No similarity of kind " + kind);
        return no_content();
      },
      Access::admin);
}

}  // namespace eos::api::ext
