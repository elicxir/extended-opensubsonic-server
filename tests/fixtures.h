#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace eos::test {

// A fresh directory under the system temp dir, removed on destruction.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool have_flac_encoder();

using Tags = std::map<std::string, std::vector<std::string>>;

// Writes a short FLAC file (sine wave at `freq_hz`, so different frequencies give
// different audio MD5s) and sets its Vorbis comments.
void make_flac(const std::filesystem::path& path, int freq_hz, const Tags& tags);

// Replaces the Vorbis comments of an existing FLAC file (the audio stays the same).
void retag_flac(const std::filesystem::path& path, const Tags& tags);

}  // namespace eos::test
