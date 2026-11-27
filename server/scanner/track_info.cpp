#include "scanner/track_info.h"

#include <algorithm>
#include <filesystem>

#include "core/ids.h"
#include "core/normalize.h"
#include "core/strings.h"

namespace eos::scanner {
namespace {

using TagMap = std::map<std::string, std::vector<std::string>>;

const std::vector<std::string>* values_of(const TagMap& tags, const std::string& key) {
  auto it = tags.find(key);
  if (it == tags.end() || it->second.empty()) return nullptr;
  return &it->second;
}

std::string first(const TagMap& tags, std::initializer_list<const char*> keys) {
  for (auto k : keys) {
    if (auto v = values_of(tags, k)) {
      for (const auto& s : *v) {
        auto t = trim(s);
        if (!t.empty()) return t;
      }
    }
  }
  return {};
}

std::vector<std::string> all(const TagMap& tags, const char* key) {
  std::vector<std::string> out;
  if (auto v = values_of(tags, key))
    for (const auto& s : *v)
      if (auto t = trim(s); !t.empty()) out.push_back(std::move(t));
  return out;
}

// Multi-valued people tags: prefer the plural tag (ARTISTS) which already holds one
// name per value; otherwise split the singular tag on the configured separators.
std::vector<Person> people(const TagMap& tags, const char* plural, const char* singular,
                           const char* sort_plural, const char* sort_singular, const char* mbid_key,
                           const std::vector<std::string>& separators) {
  std::vector<std::string> names;
  if (plural) names = all(tags, plural);
  if (names.empty()) {
    for (const auto& v : all(tags, singular))
      for (auto& n : split_any(v, separators)) names.push_back(std::move(n));
  }
  std::vector<std::string> sorts;
  if (sort_plural) sorts = all(tags, sort_plural);
  if (sorts.empty() && sort_singular) {
    for (const auto& v : all(tags, sort_singular))
      for (auto& n : split_any(v, separators)) sorts.push_back(std::move(n));
  }
  std::vector<std::string> mbids;
  if (mbid_key) {
    for (const auto& v : all(tags, mbid_key))
      for (auto& n : split_any(v, {"; ", "/"})) mbids.push_back(std::move(n));
  }
  std::vector<Person> out;
  for (size_t i = 0; i < names.size(); ++i) {
    Person p;
    p.name = names[i];
    if (sorts.size() == names.size()) p.sort_name = sorts[i];
    if (mbids.size() == names.size()) p.mbid = mbids[i];
    out.push_back(std::move(p));
  }
  return out;
}

// PERFORMER values look like "Name (instrument)".
std::vector<Person> performers(const TagMap& tags) {
  std::vector<Person> out;
  for (const auto& v : all(tags, "PERFORMER")) {
    Person p;
    auto open = v.rfind(" (");
    if (!v.empty() && v.back() == ')' && open != std::string::npos) {
      p.name = trim(v.substr(0, open));
      p.sub_role = trim(v.substr(open + 2, v.size() - open - 3));
    } else {
      p.name = v;
    }
    if (!p.name.empty()) out.push_back(std::move(p));
  }
  return out;
}

std::optional<int> year_of(const std::string& date) {
  if (date.size() >= 4) {
    auto y = parse_leading_int(date.substr(0, 4));
    if (y && *y >= 1000 && *y <= 9999) return y;
  }
  return std::nullopt;
}

}  // namespace

std::string artist_id(const std::string& name) {
  return ids::hash_id(ids::join_key({"artist", normalize(name)}));
}

std::string genre_id(const std::string& name) {
  return ids::hash_id(ids::join_key({"genre", normalize(name)}));
}

std::vector<std::string> custom_tag_values(const TagMap& t, const CustomTagConfig& tag) {
  std::vector<std::string> vals;
  for (const auto& alias : tag.aliases) {
    for (const auto& v : all(t, alias.c_str())) {
      auto pieces = tag.split.empty() ? std::vector<std::string>{v} : split_any(v, tag.split);
      for (auto& piece : pieces) {
        if (piece.size() > tag.max_length) piece.resize(tag.max_length);
        if (std::find(vals.begin(), vals.end(), piece) == vals.end()) vals.push_back(std::move(piece));
      }
    }
  }
  return vals;
}

std::string arrangement_kind(const std::string& name) {
  auto n = normalize(name);
  auto has = [&](std::initializer_list<const char*> words) {
    for (auto w : words)
      if (n.find(w) != std::string::npos) return true;
    return false;
  };
  if (n.empty()) return "original";
  if (has({"piano", "ぴあの"})) return "piano";
  if (has({"orchestra", "orchestral", "symphon", "おーけすとら", "交響"})) return "orchestra";
  if (has({"acoustic", "あこーすてぃっく"})) return "acoustic";
  if (has({"band", "rock", "ばんど"})) return "band";
  if (has({"vocal", "ぼーかる", "歌"})) return "vocal";
  return "other";
}

std::string content_type_for(const std::string& suffix) {
  if (suffix == "flac") return "audio/flac";
  if (suffix == "mp3") return "audio/mpeg";
  if (suffix == "m4a" || suffix == "mp4" || suffix == "aac") return "audio/mp4";
  if (suffix == "ogg" || suffix == "oga" || suffix == "opus") return "audio/ogg";
  if (suffix == "wav") return "audio/wav";
  return "application/octet-stream";
}

TrackInfo interpret(const RawFile& raw, const std::string& rel_path, int64_t size,
                    int64_t mtime_ms, const Config& cfg, const PidSpecs& pids) {
  const auto& t = raw.tags;
  const auto& seps = cfg.artist_separators;
  TrackInfo ti;
  ti.rel_path = rel_path;
  std::filesystem::path p(rel_path);
  ti.suffix = to_lower_ascii(p.extension().string());
  if (!ti.suffix.empty() && ti.suffix.front() == '.') ti.suffix.erase(0, 1);
  ti.content_type = content_type_for(ti.suffix);
  ti.size = size;
  ti.mtime_ms = mtime_ms;
  ti.raw = raw;

  ti.title = first(t, {"TITLE"});
  if (ti.title.empty()) ti.title = p.stem().string();
  ti.sort_title = first(t, {"TITLESORT"});
  ti.subtitle = first(t, {"SUBTITLE"});
  ti.disc = parse_leading_int(first(t, {"DISCNUMBER"}));
  ti.track = parse_leading_int(first(t, {"TRACKNUMBER"}));
  ti.disc_subtitle = first(t, {"DISCSUBTITLE"});
  ti.release_date = first(t, {"RELEASEDATE", "DATE", "YEAR"});
  ti.original_date = first(t, {"ORIGINALDATE", "ORIGINALYEAR"});
  ti.year = year_of(ti.release_date);

  ti.artists = people(t, "ARTISTS", "ARTIST", "ARTISTSSORT", "ARTISTSORT", "MUSICBRAINZ_ARTISTID", seps);
  if (ti.artists.empty()) ti.artists.push_back({kUnknownArtist, "", "", ""});
  ti.display_artist = first(t, {"ARTIST"});
  if (ti.display_artist.empty()) {
    std::vector<std::string> names;
    for (const auto& a : ti.artists) names.push_back(a.name);
    ti.display_artist = join(names, ", ");
  }
  ti.composers = people(t, nullptr, "COMPOSER", nullptr, "COMPOSERSORT", nullptr, seps);
  ti.lyricists = people(t, nullptr, "LYRICIST", nullptr, nullptr, nullptr, seps);
  ti.arrangers = people(t, nullptr, "ARRANGER", nullptr, nullptr, nullptr, seps);
  ti.conductors = people(t, nullptr, "CONDUCTOR", nullptr, nullptr, nullptr, seps);
  ti.performers = performers(t);
  {
    std::vector<std::string> names;
    for (const auto& c : ti.composers) names.push_back(c.name);
    ti.display_composer = join(names, ", ");
  }
  for (const auto& g : all(t, "GENRE"))
    for (auto& piece : split_any(g, {"; ", ";"})) ti.genres.push_back(std::move(piece));

  ti.mbz_recording_id = first(t, {"MUSICBRAINZ_TRACKID"});
  ti.mbz_track_id = first(t, {"MUSICBRAINZ_RELEASETRACKID"});
  ti.work = first(t, {"WORK"});
  ti.mbz_work_id = first(t, {"MUSICBRAINZ_WORKID"});
  if (auto kind = to_lower_ascii(first(t, {"PERFORMANCE_TYPE"})); kind == "live" || kind == "studio")
    ti.performance_kind = kind;
  ti.recorded_date = first(t, {"RECORDINGDATE"});

  for (const auto& [name, tag] : cfg.tags)
    if (auto vals = custom_tag_values(t, tag); !vals.empty()) ti.custom[name] = std::move(vals);
  ti.arrangement = first(t, {"ARRANGEMENT"});

  ti.album = first(t, {"ALBUM"});
  if (ti.album.empty()) ti.album = kUnknownAlbum;
  ti.album_sort = first(t, {"ALBUMSORT"});
  ti.album_version = first(t, {"ALBUMVERSION"});
  ti.mbz_album_id = first(t, {"MUSICBRAINZ_ALBUMID"});
  ti.compilation = first(t, {"COMPILATION"}) == "1";
  ti.album_artists = people(t, "ALBUMARTISTS", "ALBUMARTIST", "ALBUMARTISTSSORT", "ALBUMARTISTSORT",
                            "MUSICBRAINZ_ALBUMARTISTID", seps);
  ti.display_album_artist = first(t, {"ALBUMARTIST"});
  if (ti.album_artists.empty()) {
    ti.album_artists = ti.artists;
    ti.display_album_artist = ti.display_artist;
  }
  ti.album_artist_id = artist_id(ti.album_artists.front().name);

  auto raw_lookup = [&](std::string_view field) { return first(t, {to_upper_ascii(field).c_str()}); };

  ti.album_pid = compute_pid(pids.album, [&](std::string_view f) -> std::string {
    if (f == "musicbrainz_albumid") return ti.mbz_album_id;
    if (f == "albumartistid") return ti.album_artist_id;
    if (f == "album") return ti.album;
    if (f == "albumversion") return ti.album_version;
    if (f == "releasedate") return ti.release_date;
    return raw_lookup(f);
  });
  ti.track_pid = compute_pid(pids.track, [&](std::string_view f) -> std::string {
    if (f == "musicbrainz_trackid") return ti.mbz_recording_id;
    if (f == "albumid") return ti.album_pid;
    if (f == "discnumber") return ti.disc ? std::to_string(*ti.disc) : "";
    if (f == "tracknumber") return ti.track ? std::to_string(*ti.track) : "";
    if (f == "title") return ti.title;
    return raw_lookup(f);
  });
  return ti;
}

}  // namespace eos::scanner
