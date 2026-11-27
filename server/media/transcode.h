#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"

namespace eos::media {

// Produces a response body chunk by chunk. run() is called on its own thread; it calls
// `sink` with each chunk and stops early when `sink` returns false (client gone).
class StreamSource {
 public:
  virtual ~StreamSource() = default;
  virtual void run(const std::function<bool(const char*, size_t)>& sink) = 0;
};

struct TranscodePlan {
  std::string format;        // "opus", "mp3"
  std::string suffix;        // file suffix of the output
  std::string content_type;  // MIME type of the output
  int bitrate_kbps = 0;
  int64_t offset_seconds = 0;
};

// Decides whether a stream request needs transcoding (design: 配信とトランスコード).
// Returns nullopt when the original file should be sent.
std::optional<TranscodePlan> plan_transcode(const Config& cfg, const std::string& source_suffix,
                                            int source_kbps, const std::optional<std::string>& format,
                                            const std::optional<int64_t>& max_kbps,
                                            int64_t time_offset_seconds);

// Builds the ffmpeg argv for a plan (no shell is involved).
std::vector<std::string> ffmpeg_args(const Config& cfg, const TranscodePlan& plan,
                                     const std::filesystem::path& input);

class Transcoder {
 public:
  explicit Transcoder(const Config& cfg) : cfg_(cfg) {}

  // Starts ffmpeg. Returns nullptr when transcode.max_concurrent processes are running.
  std::shared_ptr<StreamSource> start(const TranscodePlan& plan, const std::filesystem::path& input);
  int running() const { return running_.load(); }

 private:
  const Config& cfg_;
  std::atomic<int> running_{0};
};

}  // namespace eos::media
