#pragma once

#include <string>

#include "api/ext/router.h"

namespace eos::api::ext {

// A suggestion for reading: rule, method, score, status, a one-line summary, the items
// it touches (with names) and the proposal itself.
json suggestion_json(db::Connection& db, const std::string& id);

// Checks a proposal before it is stored or applied. Throws bad_request / not_found.
// Proposal actions:
//   link_composition    {name, composers[], tracks[], compositionId?}
//   merge_arrangements  {arrangementId, tracks[]}
//   relation            {type, subtype?, from{type,id}, to{type,id}, position?, note?}
//   medley              {from{type,id}, parts[{position, to{type,id}}]}
//   link_title          {tracks[], titleId? | titleName}
//   virtual_album       {name, tracks[] (in order)}
void validate_proposal(db::Connection& db, const nlohmann::json& proposal);

// Writes what a proposal says, as rows of `source` (1 = accepted estimate, 3 = edited
// by hand) pointing back to `suggestion_id`. Run inside a transaction.
void apply_proposal(db::Connection& w, const nlohmann::json& proposal, int source, const std::string& suggestion_id);

}  // namespace eos::api::ext
