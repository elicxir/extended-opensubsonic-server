#include "api/rate_limit.h"

namespace eos::api {

RateLimiter::RateLimiter(int max_failures, std::chrono::seconds window, std::chrono::seconds block)
    : max_failures_(max_failures), window_(window), block_(block) {}

void RateLimiter::prune(Entry& e, Clock::time_point now) {
  while (!e.failures.empty() && now - e.failures.front() > window_) e.failures.pop_front();
}

bool RateLimiter::blocked(const std::string& key, Clock::time_point now) {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(key);
  return it != entries_.end() && now < it->second.blocked_until;
}

void RateLimiter::failure(const std::string& key, Clock::time_point now) {
  std::lock_guard lock(mutex_);
  if (entries_.size() > 100'000) entries_.clear();  // memory bound against floods
  auto& e = entries_[key];
  prune(e, now);
  e.failures.push_back(now);
  if (static_cast<int>(e.failures.size()) >= max_failures_) {
    e.blocked_until = now + block_;
    e.failures.clear();
  }
}

void RateLimiter::success(const std::string& key) {
  std::lock_guard lock(mutex_);
  entries_.erase(key);
}

}  // namespace eos::api
