#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/database.h"

namespace eos::scanner {

// How well the library is tagged, to decide how much MusicBrainz / VGMdb data there is
// and how much has to come from estimates (design: 未決事項).
struct TagReport {
  struct Row {
    std::string tag;
    int64_t tracks = 0;
    double share = 0;  // of all live tracks, 0..1
  };
  int64_t tracks = 0;
  int64_t albums = 0;
  std::vector<Row> key_tags;  // the tags the design relies on, in a fixed order
  std::vector<Row> all_tags;  // every tag present, most common first
};

TagReport build_tag_report(db::Connection& conn);
std::string format_tag_report(const TagReport& report);
std::string tag_report_json(const TagReport& report);

}  // namespace eos::scanner
