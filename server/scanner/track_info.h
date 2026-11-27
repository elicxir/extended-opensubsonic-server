#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "scanner/pid.h"
#include "scanner/tag_reader.h"

namespace eos::scanner {

inline constexpr const char* kUnknownAlbum = "[Unknown Album]";
inline constexpr const char* kUnknownArtist = "[Unknown Artist]";

struct Person {
  std::string name;
  std::string sort_name;
  std::string mbid;
  std::string sub_role;  // instrument for performers
};

// One file's tags, interpreted with the rules in the design's tag table.
struct TrackInfo {
  // file
  std::string rel_path;
  std::string suffix;
  std::string content_type;
  int64_t size = 0;
  int64_t mtime_ms = 0;
  RawFile raw;

  // track
  std::string title, sort_title, subtitle;
  std::optional<int> disc, track;
  std::string disc_subtitle;
  std::string release_date, original_date;  // release_date falls back to DATE for display
  std::string release_date_tag;             // RELEASEDATE only: used for the album PID
  std::optional<int> year;
  std::vector<Person> artists, composers, lyricists, arrangers, conductors, performers;
  std::string display_artist, display_composer;
  std::vector<std::string> genres;
  std::string mbz_recording_id, mbz_track_id, work, mbz_work_id;
  std::optional<std::string> performance_kind;
  std::string recorded_date;
  std::string arrangement;  // ARRANGEMENT tag
  // ReplayGain in dB / linear peak (REPLAYGAIN_* tags, or R128_* converted to the
  // ReplayGain 2.0 reference of -18 LUFS).
  std::optional<double> rg_track_gain, rg_track_peak, rg_album_gain, rg_album_peak;
  std::map<std::string, std::vector<std::string>> custom;  // config tag name -> values

  // album
  std::string album, album_sort, album_version, mbz_album_id;
  std::vector<Person> album_artists;
  std::string display_album_artist;
  bool compilation = false;

  // derived
  std::string album_artist_id;
  std::string album_pid;
  std::string track_pid;
};

struct PidSpecs {
  PidSpec track;
  PidSpec album;
  static PidSpecs from(const Config& cfg) {
    return {PidSpec::parse(cfg.pid_track), PidSpec::parse(cfg.pid_album)};
  }
};

TrackInfo interpret(const RawFile& raw, const std::string& rel_path, int64_t size,
                    int64_t mtime_ms, const Config& cfg, const PidSpecs& pids);

// Artist ids are derived from the normalised name so that the same name always maps
// to the same artist across scans.
std::string artist_id(const std::string& name);
std::string genre_id(const std::string& name);

std::string content_type_for(const std::string& suffix);

// Values of a custom tag (all aliases, split and truncated as configured).
std::vector<std::string> custom_tag_values(const std::map<std::string, std::vector<std::string>>& tags,
                                           const CustomTagConfig& tag);

// Guesses an arrangement kind from its name ("Piano Ver." -> "piano").
std::string arrangement_kind(const std::string& name);

}  // namespace eos::scanner
