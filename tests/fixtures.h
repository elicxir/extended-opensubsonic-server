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

// Embeds a picture (front cover) into an existing FLAC file.
void add_flac_picture(const std::filesystem::path& path, const std::string& data,
                      const std::string& mime);

// A small solid-colour PNG (w x h), for cover-art tests.
std::string make_png(int w, int h);

}  // namespace eos::test
