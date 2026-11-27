#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/database.h"

namespace eos::search {

// Rebuilds search_index (FTS5, trigram) from tracks, albums, artists, compositions and
// works. Every value goes through eos_norm; the parts of one column are joined with
// U+0001 so a match never spans two values. Run inside a transaction.
void rebuild_index(db::Connection& c);

// Splits a query into normalised words (empty words dropped).
std::vector<std::string> query_terms(const std::string& query);

// What to search and how to filter / order the hits of one kind.
struct Target {
  std::string kind;            // search_index.kind
  std::string join;            // e.g. "JOIN album e ON e.id = search_index.ref_id"
  std::string filter;          // SQL on `e`, e.g. "e.missing = 0"
  std::string annotation;      // annotation.item_type, or empty
  std::string order;           // tie-breaker, e.g. "COALESCE(e.sort_name, e.name)"
};

// Ids of `target.kind` matching every term, ranked: exact name, name prefix, name
// contains, reading/alias contains, other fields; then play count and stars.
// Terms of 3+ characters use the trigram index; shorter ones fall back to instr().
std::vector<std::string> find(db::Connection& c, const Target& target, const std::vector<std::string>& terms,
                              int64_t user_id, int64_t count, int64_t offset);

}  // namespace eos::search
