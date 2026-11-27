#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace eos::scanner {

// A PID spec like "musicbrainz_trackid|albumid,discnumber,tracknumber,title":
// alternatives separated by '|', each a comma-separated list of fields.
struct PidSpec {
  std::vector<std::vector<std::string>> alternatives;
  static PidSpec parse(std::string_view spec);
};

using FieldLookup = std::function<std::string(std::string_view field)>;

// Uses the first alternative whose fields are all non-empty (the last one when none
// qualifies), normalises each value and hashes "field=value" pairs into a 22-char id.
std::string compute_pid(const PidSpec& spec, const FieldLookup& lookup);

}  // namespace eos::scanner
