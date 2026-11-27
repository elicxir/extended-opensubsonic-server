// search3. M2 matches with LIKE on normalised text (eos_norm); FTS5 replaces it in M3.
#include "api/subsonic/builders.h"
#include "api/subsonic/router.h"
#include "core/normalize.h"
#include "core/strings.h"

namespace eos::api {
namespace {

// Splits the query into words and normalises each. Quotes and '*' wildcards that
// some clients send are dropped by the normalisation.
std::vector<std::string> query_terms(const std::string& q) {
  std::vector<std::string> out;
  for (const auto& w : split_any(q, {" ", "\t", "　"}))
    if (auto n = normalize(w); !n.empty()) out.push_back(std::move(n));
  return out;
}

// WHERE fragment: every term must appear in the concatenation of `fields`, and the
// ORDER BY rank: exact match on the first field, then prefix, then the rest.
struct Match {
  std::string where;
  std::string rank;
  std::vector<std::string> binds;
};

Match build_match(const std::vector<std::string>& terms, const std::vector<std::string>& fields) {
  Match m;
  if (terms.empty()) {
    m.where = "1";
    m.rank = "NULL";  // a bare integer in ORDER BY would mean a column number
    return m;
  }
  std::string hay;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) hay += " || ' ' || ";
    hay += "eos_norm(COALESCE(" + fields[i] + ", ''))";
  }
  for (size_t i = 0; i < terms.size(); ++i) {
    if (i) m.where += " AND ";
    m.where += "instr(" + hay + ", ?) > 0";
    m.binds.push_back(terms[i]);
  }
  std::string whole;
  for (const auto& t : terms) whole += t;
  const std::string primary = "eos_norm(COALESCE(" + fields[0] + ", ''))";
  m.rank = "CASE WHEN " + primary + " = ? THEN 0 WHEN substr(" + primary +
           ", 1, length(?)) = ? THEN 1 WHEN instr(" + primary + ", ?) > 0 THEN 2 ELSE 3 END";
  for (int i = 0; i < 4; ++i) m.binds.push_back(whole);
  return m;
}

std::vector<std::string> run_search(db::Connection& db, const std::string& select_from,
                                    const std::string& filter, const Match& m,
                                    const std::string& order_tail, int64_t count, int64_t offset) {
  std::string sql = select_from + " WHERE " + filter + " AND (" + m.where + ") ORDER BY " + m.rank +
                    ", " + order_tail + " LIMIT ? OFFSET ?";
  auto st = db.prepare(sql);
  int i = 0;
  // where binds, then rank binds (both are in m.binds in that order)
  for (const auto& b : m.binds) st.bind(++i, b);
  st.bind(++i, count);
  st.bind(++i, offset);
  std::vector<std::string> ids;
  while (st.step()) ids.push_back(st.text(0));
  return ids;
}

}  // namespace

void add_search_handlers(HandlerTable& t) {
  t.add("search3", [](Ctx& c, Node& root) {
    auto terms = query_terms(c.param("query").value_or(""));
    Loader loader(c.db, c.user.id);
    Node n("searchResult3");

    auto artist_count = c.int_param("artistCount", 20, 0, 500);
    auto album_count = c.int_param("albumCount", 20, 0, 500);
    // An empty query lists everything; clients that mirror the library page through
    // songs this way, so allow large pages.
    auto song_count = c.int_param("songCount", 20, 0, terms.empty() ? 5000 : 500);

    if (artist_count > 0) {
      auto m = build_match(terms, {"ar.name", "ar.sort_name"});
      auto ids = run_search(
          c.db, "SELECT ar.id FROM artist ar",
          "EXISTS (SELECT 1 FROM participant p JOIN album al ON al.id = p.entity_id "
          "WHERE p.entity_type = 'album' AND p.role = 'albumartist' AND p.artist_id = ar.id "
          "AND al.missing = 0 AND al.kind = 'physical')",
          m, "COALESCE(ar.sort_name, ar.name) COLLATE NOCASE", artist_count,
          c.int_param("artistOffset", 0, 0, INT64_MAX));
      n.list("artist", loader.artists(ids));
    }
    if (album_count > 0) {
      auto m = build_match(terms, {"al.name", "al.sort_name", "al.display_artist"});
      auto ids = run_search(c.db, "SELECT al.id FROM album al", "al.missing = 0 AND al.kind = 'physical'",
                            m, "COALESCE(al.sort_name, al.name) COLLATE NOCASE, al.id", album_count,
                            c.int_param("albumOffset", 0, 0, INT64_MAX));
      n.list("album", loader.albums(ids));
    }
    if (song_count > 0) {
      auto m = build_match(terms, {"t.title", "t.sort_title", "t.subtitle", "al.name", "t.display_artist"});
      auto ids = run_search(c.db, "SELECT t.id FROM track t JOIN album al ON al.id = t.album_id",
                            "t.missing = 0", m,
                            terms.empty() ? "t.album_id, COALESCE(t.disc_number, 0), "
                                            "COALESCE(t.track_number, 0), t.path"
                                          : "COALESCE(t.sort_title, t.title) COLLATE NOCASE, t.id",
                            song_count, c.int_param("songOffset", 0, 0, INT64_MAX));
      n.list("song", loader.songs(ids));
    }
    root.child(std::move(n));
  });
}

}  // namespace eos::api
