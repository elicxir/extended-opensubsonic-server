#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace eos::api {

// Counts failed logins per key (an IP or a username). After `max_failures` within
// `window`, the key is blocked for `block`. A success clears the key.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  RateLimiter(int max_failures = 10, std::chrono::seconds window = std::chrono::minutes(10),
              std::chrono::seconds block = std::chrono::minutes(15));

  bool blocked(const std::string& key, Clock::time_point now = Clock::now());
  void failure(const std::string& key, Clock::time_point now = Clock::now());
  void success(const std::string& key);

 private:
  struct Entry {
    std::deque<Clock::time_point> failures;
    Clock::time_point blocked_until{};
  };
  void prune(Entry& e, Clock::time_point now);

  int max_failures_;
  std::chrono::seconds window_, block_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace eos::api
