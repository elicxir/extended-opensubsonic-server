#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace eos::scanner {

// What TagLib gives us for one file, before any interpretation.
struct RawFile {
  std::map<std::string, std::vector<std::string>> tags;  // upper-case keys, in file order
  int64_t duration_ms = 0;
  int bit_rate = 0;  // kbps
  int sample_rate = 0;
  int channels = 0;
  std::optional<int> bit_depth;
  std::optional<std::string> audio_md5;  // FLAC STREAMINFO MD5, hex; absent when unset
  bool has_picture = false;
};

// Reads tags and audio properties. Returns nullopt and sets `error` when the file
// cannot be parsed.
std::optional<RawFile> read_file(const std::filesystem::path& path, std::string& error);

}  // namespace eos::scanner
