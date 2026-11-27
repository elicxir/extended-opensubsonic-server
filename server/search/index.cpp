#include "search/index.h"

#include <utf8proc.h>

#include "core/normalize.h"
#include "core/strings.h"

namespace eos::search {
namespace {

size_t codepoints(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

}  // namespace

void rebuild_index(db::Connection& c) {
  // char(1) separates values inside one column; eos_norm removes nothing of it
  // because it is applied to each value first.
  c.exec(R"SQL(
DELETE FROM search_index;

INSERT INTO search_index (kind, ref_id, name, alt, context)
SELECT 'track', t.id, eos_norm(t.title),
       eos_norm(COALESCE(t.sort_title, '')) || char(1) || eos_norm(COALESCE(t.subtitle, '')),
       eos_norm(COALESCE(t.display_artist, '')) || char(1) || eos_norm(al.name) || char(1) ||
       eos_norm(COALESCE(t.display_composer, ''))
FROM track t JOIN album al ON al.id = t.album_id WHERE t.missing = 0;

INSERT INTO search_index (kind, ref_id, name, alt, context)
SELECT 'album', al.id, eos_norm(al.name), eos_norm(COALESCE(al.sort_name, '')),
       eos_norm(COALESCE(al.display_artist, ''))
FROM album al WHERE al.missing = 0;

INSERT INTO search_index (kind, ref_id, name, alt, context)
SELECT 'artist', ar.id, eos_norm(ar.name), eos_norm(COALESCE(ar.sort_name, '')), ''
FROM artist ar;

INSERT INTO search_index (kind, ref_id, name, alt, context)
SELECT 'composition', co.id, eos_norm(co.name), eos_norm(COALESCE(co.sort_name, '')),
       COALESCE((SELECT group_concat(eos_norm(a.name), char(1)) FROM participant p
                 JOIN artist a ON a.id = p.artist_id
                 WHERE p.entity_type = 'composition' AND p.entity_id = co.id AND p.role = 'composer'), '')
FROM composition co;

INSERT INTO search_index (kind, ref_id, name, alt, context)
SELECT 'title', ti.id, eos_norm(ti.name), eos_norm(COALESCE(ti.sort_name, '')), ''
FROM title ti;
)SQL");
}

std::vector<std::string> query_terms(const std::string& q) {
  std::vector<std::string> out;
  for (const auto& w : split_any(q, {" ", "\t", "　"}))
    if (auto n = normalize(w); !n.empty()) out.push_back(std::move(n));
  return out;
}

std::vector<std::string> find(db::Connection& c, const Target& t, const std::vector<std::string>& terms,
                              int64_t user_id, int64_t count, int64_t offset) {
  std::vector<std::string> binds;
  std::string where = "search_index.kind = ?";
  binds.push_back(t.kind);

  std::string match;
  for (const auto& term : terms) {
    if (codepoints(term) >= 3) {
      if (!match.empty()) match += ' ';
      match += '"' + term + '"';  // normalised terms contain no quotes
    } else {
      where += " AND instr(search_index.name || char(1) || search_index.alt || char(1) || "
               "search_index.context, ?) > 0";
      binds.push_back(term);
    }
  }
  if (!match.empty()) {
    where += " AND search_index MATCH ?";
    binds.push_back(match);
  }
  if (!t.filter.empty()) where += " AND (" + t.filter + ")";

  std::string whole;
  for (const auto& term : terms) whole += term;
  std::string rank =
      "CASE WHEN search_index.name = ? THEN 0 "
      "WHEN substr(search_index.name, 1, length(?)) = ? THEN 1 "
      "WHEN instr(search_index.name, ?) > 0 THEN 2 "
      "WHEN instr(search_index.alt, ?) > 0 THEN 3 ELSE 4 END";

  std::string sql = "SELECT search_index.ref_id FROM search_index " + t.join;
  if (!t.annotation.empty())
    sql += " LEFT JOIN annotation an ON an.user_id = ? AND an.item_type = '" + t.annotation +
           "' AND an.item_id = search_index.ref_id";
  sql += " WHERE " + where + " ORDER BY " + rank;
  if (!t.annotation.empty()) sql += ", COALESCE(an.play_count, 0) DESC, an.starred_at IS NULL";
  sql += ", " + t.order + " COLLATE NOCASE, search_index.ref_id LIMIT ? OFFSET ?";

  auto st = c.prepare(sql);
  int i = 0;
  if (!t.annotation.empty()) st.bind(++i, user_id);
  for (const auto& b : binds) st.bind(++i, b);
  for (int k = 0; k < 5; ++k) st.bind(++i, whole);
  st.bind(++i, count);
  st.bind(++i, offset);
  std::vector<std::string> ids;
  while (st.step()) ids.push_back(st.text(0));
  return ids;
}

}  // namespace eos::search
