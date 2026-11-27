#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "db/write_queue.h"

namespace eos::scanner {

struct ScanStats {
  int64_t seen = 0;       // audio files found on disk
  int64_t added = 0;      // new tracks
  int64_t updated = 0;    // same path, tags or file changed
  int64_t moved = 0;      // new path matched to an existing track
  int64_t restored = 0;   // a missing track came back
  int64_t missing = 0;    // tracks newly marked missing
  int64_t unchanged = 0;
  int64_t purged = 0;     // missing tracks deleted after the grace period
  std::vector<std::string> errors;
};

// Progress for getScanStatus / /ext/scan.
struct ScanStatus {
  bool scanning = false;
  std::string phase;
  int64_t count = 0;          // files processed in this scan
  int64_t folder_count = 0;   // directories visited in this scan
  int64_t last_scan_ms = 0;
};

class Scanner {
 public:
  Scanner(const Config& cfg, db::WriteQueue& writer, std::filesystem::path db_path);

  // Runs one scan of every library. `full` re-reads every file. Only one scan runs at
  // a time; a second caller gets the result of waiting for the lock.
  ScanStats scan(bool full);

  ScanStatus status() const;

 private:
  ScanStats scan_library(const LibraryConfig& lib, bool full, int64_t run_started);
  void set_phase(const std::string& phase);

  const Config& cfg_;
  db::WriteQueue& writer_;
  std::filesystem::path db_path_;
  std::mutex scan_mutex_;
  mutable std::mutex status_mutex_;
  ScanStatus status_;
  std::atomic<int64_t> count_{0};
  std::atomic<int64_t> folders_{0};
};

}  // namespace eos::scanner
