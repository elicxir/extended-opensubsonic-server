#pragma once

#include <optional>
#include <string>

#include "core/config.h"
#include "db/database.h"

namespace eos::suggest {

// Runs the rule-based suggestion rules over the whole library (design: 候補を出す規則):
//   same_composition     tracks without a composition, same base title and composers
//   performance_variant  same composition, base title and arrangers, split over arrangements
//   derivative           a "(TV Size)" / "Instrumental" / ... track and its original
//   medley               a title made of other titles joined by 〜, / or -
// New candidates are inserted as pending; pending rule suggestions that no longer
// apply become stale. Rejected ones are never raised again (fingerprint). Returns the
// number of new suggestions. Run inside a transaction.
int64_t run_rules(db::Connection& c, const Config& cfg);

// The title with derivative markers and trailing bracketed notes removed, normalised.
std::string base_title(const std::string& title, const Config& cfg);
// The derivative subtype a title is marked with (tv_size, instrumental, off_vocal,
// remix, other), or nullopt when it has no marker.
std::optional<std::string> derivative_subtype(const std::string& title, const Config& cfg);

}  // namespace eos::suggest
