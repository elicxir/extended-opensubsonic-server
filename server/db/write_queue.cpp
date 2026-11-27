#include "db/write_queue.h"

#include <stdexcept>

#include "core/log.h"

namespace eos::db {

WriteQueue::WriteQueue(const std::filesystem::path& path) : path_(path) {
  auto ready = ready_.get_future();
  thread_ = std::thread([this] { loop(); });
  ready.get();  // rethrows if the connection could not be opened
}

WriteQueue::~WriteQueue() { stop(); }

void WriteQueue::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void WriteQueue::enqueue(std::function<void(Connection&)> job) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("WriteQueue is stopped");
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void WriteQueue::loop() {
  std::unique_ptr<Connection> conn;
  try {
    conn = std::make_unique<Connection>(path_);
  } catch (...) {
    ready_.set_exception(std::current_exception());
    return;
  }
  ready_.set_value();
  while (true) {
    std::function<void(Connection&)> job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;  // stopping and drained
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    // packaged_task stores exceptions in the future; anything else is a bug worth logging.
    try {
      job(*conn);
    } catch (const std::exception& e) {
      log::error("write queue: job failed: {}", e.what());
    }
  }
}

}  // namespace eos::db
