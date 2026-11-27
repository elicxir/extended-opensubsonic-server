#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

#include "core/config.h"
#include "db/database.h"

namespace eos::media {

struct Image {
  std::string mime;
  std::string data;
};

// Finds and (optionally) shrinks cover art (design: ジャケット).
//   album: an image file in the album folder (artwork.album_files order), else the
//          embedded picture of the album's first track
//   track: the track's embedded picture, else its album's art
// Resized images are JPEG, cached under <data_dir>/cache/artwork.
class Artwork {
 public:
  explicit Artwork(const Config& cfg);

  std::optional<Image> album(db::Connection& db, const std::string& album_id, int size);
  std::optional<Image> track(db::Connection& db, const std::string& track_id, int size);

  static std::optional<Image> embedded(const std::filesystem::path& file);
  // Longest side becomes `size` (never enlarges). Returns nullopt if not decodable.
  static std::optional<Image> resize(const Image& in, int size);

 private:
  std::optional<Image> finish(Image img, const std::string& source_key, int size);
  std::optional<std::filesystem::path> folder_image(const std::filesystem::path& dir) const;
  void evict_if_needed();

  const Config& cfg_;
  std::filesystem::path cache_dir_;
  std::atomic<int> writes_since_evict_{0};
};

// Absolute path of a track's file, checked to stay inside its library root.
std::optional<std::filesystem::path> track_file(db::Connection& db, const std::string& track_id);

}  // namespace eos::media
