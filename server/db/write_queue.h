#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>

#include "db/database.h"

namespace eos::db {

// Every write to the database goes through this queue: one thread, one read-write
// connection. Callers submit a function that receives the connection and get a future
// for its result. Jobs run in submission order; a job is responsible for its own
// transaction (see Transaction).
class WriteQueue {
 public:
  explicit WriteQueue(const std::filesystem::path& path);
  ~WriteQueue();
  WriteQueue(const WriteQueue&) = delete;
  WriteQueue& operator=(const WriteQueue&) = delete;

  template <class F>
  auto submit(F&& fn) -> std::future<std::invoke_result_t<F, Connection&>> {
    using R = std::invoke_result_t<F, Connection&>;
    auto task = std::make_shared<std::packaged_task<R(Connection&)>>(std::forward<F>(fn));
    auto fut = task->get_future();
    enqueue([task](Connection& c) { (*task)(c); });
    return fut;
  }

  // Convenience: submit and wait.
  template <class F>
  auto run(F&& fn) {
    return submit(std::forward<F>(fn)).get();
  }

  // Stops accepting work after draining the queue. Called by the destructor.
  void stop();

 private:
  void enqueue(std::function<void(Connection&)> job);
  void loop();

  std::filesystem::path path_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void(Connection&)>> jobs_;
  bool stopping_ = false;
  std::thread thread_;
  std::promise<void> ready_;
};

}  // namespace eos::db
