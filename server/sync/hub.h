#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "db/write_queue.h"

namespace eos::sync {

// Where messages for one connected device go (a WebSocket in production).
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void send(const std::string& text) = 0;
};

struct Session {
  int64_t user_id = 0;
  std::shared_ptr<Sink> sink;
  std::string device_id;  // set by "hello"
};
using SessionPtr = std::shared_ptr<Session>;

// Shared remote control (design: リアルタイム同期). The server keeps, per user, one
// queue and playback state; devices send edits and commands, which are applied in
// arrival order (last write wins), and every device gets the result. One device - the
// "active" one - makes the sound and reports its position; the others only display.
//
// Device -> server:  hello, queue.edit, control (incl. volume / mute), progress,
//                    transfer
// Server -> device:  state (whole queue + playback), playback, devices, error
//
// The queue is written to the database before it is broadcast; positions are written
// on pause / transfer / disconnect and every `flush_every` while playing. Volume is
// per device (stored in `device`), and playback reports the active device's volume.
// Play counts are not derived here: the active device calls /rest/scrobble itself.
class SessionHub {
 public:
  SessionHub(db::WriteQueue& writer, std::chrono::milliseconds flush_every = std::chrono::seconds(30));
  ~SessionHub();
  SessionHub(const SessionHub&) = delete;
  SessionHub& operator=(const SessionHub&) = delete;

  SessionPtr connect(int64_t user_id, std::shared_ptr<Sink> sink);
  void receive(const SessionPtr& session, std::string_view text);
  void disconnect(const SessionPtr& session);

  // The queue was replaced outside the hub (REST savePlayQueue): reload and broadcast.
  void queue_replaced(int64_t user_id);
  // Writes the user's current position now (before REST getPlayQueue reads it).
  void flush(int64_t user_id);
  // Writes every unsaved position (at shutdown, before the WriteQueue stops).
  void flush_all();

  // The user's state message, for tests and diagnostics.
  nlohmann::json state_message(int64_t user_id);

 private:
  struct Entry {
    std::string entry_id, track_id;
  };
  struct Device {
    std::string name, client;
    int connections = 0;
    int volume = 100;  // 0-100
    bool muted = false;
  };
  struct UserState {
    bool loaded = false;
    int64_t revision = 0;
    std::vector<Entry> queue;
    std::optional<std::string> current;  // entry id
    int64_t position_ms = 0;
    int64_t at_ms = 0;                     // when position_ms was true
    bool playing = false;
    std::string active_device;
    std::string changed_by;
    std::map<std::string, Device> devices;  // device id -> device
    std::vector<std::weak_ptr<Session>> sessions;
    bool position_dirty = false;
    int64_t persisted_at = 0;
  };

  UserState& user(int64_t user_id);  // loads on first use; mutex held
  void load(int64_t user_id, UserState& u);
  void persist_queue(int64_t user_id, UserState& u);
  void persist_position(int64_t user_id, UserState& u);

  void on_hello(const SessionPtr& s, UserState& u, const nlohmann::json& m);
  void on_queue_edit(const SessionPtr& s, UserState& u, const nlohmann::json& m);
  void on_control(const SessionPtr& s, UserState& u, const nlohmann::json& m);
  void on_progress(const SessionPtr& s, UserState& u, const nlohmann::json& m);
  void on_transfer(const SessionPtr& s, UserState& u, const nlohmann::json& m);

  nlohmann::json playback_fields(const UserState& u) const;
  nlohmann::json state_json(const UserState& u) const;
  nlohmann::json playback_json(const UserState& u) const;
  nlohmann::json devices_json(const UserState& u) const;
  void broadcast(UserState& u, const nlohmann::json& message);
  static void send(const SessionPtr& s, const nlohmann::json& message);
  static void error(const SessionPtr& s, const std::string& message);
  int64_t position_now(const UserState& u) const;
  std::optional<size_t> index_of(const UserState& u, const std::string& entry_id) const;

  void flusher();
  void persist_volume(const std::string& device_id, const Device& d);

  db::WriteQueue& writer_;
  const std::chrono::milliseconds flush_every_;
  std::mutex mutex_;
  std::map<int64_t, UserState> users_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread flusher_;
};

int64_t now_ms();

}  // namespace eos::sync
