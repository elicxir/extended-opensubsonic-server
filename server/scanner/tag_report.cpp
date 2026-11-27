#include "scanner/tag_report.h"

#include <format>
#include <nlohmann/json.hpp>

namespace eos::scanner {
namespace {
const char* const kKeyTags[] = {
    "MUSICBRAINZ_TRACKID", "MUSICBRAINZ_ALBUMID", "MUSICBRAINZ_ARTISTID", "MUSICBRAINZ_WORKID",
    "WORK",                "COMPOSER",            "ARRANGER",             "PERFORMER",
    "CONDUCTOR",           "ALBUMARTIST",         "DISCSUBTITLE",         "TITLESORT",
    "ARTISTSORT",          "ALBUMSORT",           "GENRE",                "DATE",
    "ORIGINALDATE",        "COLLECTION_WORK",     "COLLECTION",           "COLLECTION_ALBUM",
    "ARRANGEMENT",         "PERFORMANCE_TYPE",    "RECORDINGDATE",
};
}

TagReport build_tag_report(db::Connection& conn) {
  TagReport r;
  r.tracks = conn.scalar_int("SELECT COUNT(*) FROM track WHERE missing = 0").value_or(0);
  r.albums = conn.scalar_int("SELECT COUNT(*) FROM album WHERE missing = 0 AND kind = 'physical'")
                 .value_or(0);
  auto share = [&](int64_t n) { return r.tracks ? static_cast<double>(n) / r.tracks : 0.0; };

  auto st = conn.prepare(
      "SELECT j.key, COUNT(DISTINCT t.id) FROM track t, json_each(t.tags_json) j "
      "WHERE t.missing = 0 GROUP BY j.key ORDER BY 2 DESC, 1");
  std::vector<TagReport::Row> all;
  while (st.step()) all.push_back({st.text(0), st.int64(1), share(st.int64(1))});
  for (auto key : kKeyTags) {
    int64_t n = 0;
    for (const auto& row : all)
      if (row.tag == key) n = row.tracks;
    r.key_tags.push_back({key, n, share(n)});
  }
  r.all_tags = std::move(all);
  return r;
}

std::string format_tag_report(const TagReport& r) {
  std::string out = std::format("tracks: {}\nalbums: {}\n\nkey tags\n", r.tracks, r.albums);
  for (const auto& row : r.key_tags)
    out += std::format("  {:<22} {:>7}  {:5.1f}%\n", row.tag, row.tracks, row.share * 100);
  out += "\nall tags\n";
  for (const auto& row : r.all_tags)
    out += std::format("  {:<32} {:>7}  {:5.1f}%\n", row.tag, row.tracks, row.share * 100);
  return out;
}

std::string tag_report_json(const TagReport& r) {
  auto rows = [](const std::vector<TagReport::Row>& v) {
    auto a = nlohmann::json::array();
    for (const auto& row : v) a.push_back({{"tag", row.tag}, {"tracks", row.tracks}, {"share", row.share}});
    return a;
  };
  nlohmann::json j = {{"tracks", r.tracks},
                      {"albums", r.albums},
                      {"keyTags", rows(r.key_tags)},
                      {"allTags", rows(r.all_tags)}};
  return j.dump(2);
}

}  // namespace eos::scanner
