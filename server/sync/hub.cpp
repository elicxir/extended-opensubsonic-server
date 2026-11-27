#include "sync/hub.h"

#include <algorithm>

#include "core/ids.h"
#include "core/log.h"
#include "db/database.h"

namespace eos::sync {

using json = nlohmann::json;

int64_t now_ms() { return db::now_ms(); }

SessionHub::SessionHub(db::WriteQueue& writer, std::chrono::milliseconds flush_every)
    : writer_(writer), flush_every_(flush_every), flusher_([this] { flusher(); }) {}

SessionHub::~SessionHub() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (flusher_.joinable()) flusher_.join();
  std::lock_guard lock(mutex_);
  for (auto& [id, u] : users_)
    if (u.loaded && u.position_dirty) {
      try {
        persist_position(id, u);
      } catch (const std::exception& e) {
        log::warn("sync: could not save position for user {}: {}", id, e.what());
      }
    }
}

// ---- state ----

SessionHub::UserState& SessionHub::user(int64_t user_id) {
  auto& u = users_[user_id];
  if (!u.loaded) load(user_id, u);
  return u;
}

void SessionHub::load(int64_t user_id, UserState& u) {
  writer_.run([&](db::Connection& c) {
    u.queue.clear();
    u.current.reset();
    auto st = c.prepare(
        "SELECT revision, current_index, position_ms, COALESCE(active_device_id, ''), COALESCE(changed_by, '') "
        "FROM play_queue WHERE user_id = ?");
    st.bind(1, user_id);
    std::optional<int64_t> current_index;
    if (st.step()) {
      u.revision = st.int64(0);
      current_index = st.opt_int64(1);
      u.position_ms = st.int64(2);
      if (u.active_device.empty()) u.active_device = st.text(3);
      u.changed_by = st.text(4);
    }
    auto e = c.prepare("SELECT entry_id, track_id FROM play_queue_entry WHERE user_id = ? ORDER BY position");
    e.bind(1, user_id);
    while (e.step()) u.queue.push_back({e.text(0), e.text(1)});
    if (current_index && *current_index >= 0 && *current_index < static_cast<int64_t>(u.queue.size()))
      u.current = u.queue[static_cast<size_t>(*current_index)].entry_id;
  });
  u.at_ms = now_ms();
  u.loaded = true;
}

void SessionHub::persist_queue(int64_t user_id, UserState& u) {
  std::optional<int64_t> index;
  if (u.current)
    if (auto i = index_of(u, *u.current)) index = static_cast<int64_t>(*i);
  auto position = position_now(u);
  writer_.run([&](db::Connection& c) {
    db::Transaction tx(c);
    c.run(
        "INSERT INTO play_queue (user_id, revision, current_index, position_ms, active_device_id, changed_at, "
        "changed_by) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) ON CONFLICT(user_id) DO UPDATE SET revision = ?2, "
        "current_index = ?3, position_ms = ?4, active_device_id = ?5, changed_at = ?6, changed_by = ?7",
        user_id, u.revision, index, position,
        u.active_device.empty() ? std::optional<std::string>() : u.active_device, now_ms(), u.changed_by);
    c.run("DELETE FROM play_queue_entry WHERE user_id = ?", user_id);
    int64_t pos = 0;
    for (const auto& e : u.queue)
      c.run("INSERT INTO play_queue_entry (user_id, position, entry_id, track_id) VALUES (?, ?, ?, ?)", user_id, pos++,
            e.entry_id, e.track_id);
    tx.commit();
  });
  u.position_dirty = false;
  u.persisted_at = now_ms();
}

void SessionHub::persist_position(int64_t user_id, UserState& u) {
  std::optional<int64_t> index;
  if (u.current)
    if (auto i = index_of(u, *u.current)) index = static_cast<int64_t>(*i);
  auto position = position_now(u);
  writer_.run([&](db::Connection& c) {
    c.run(
        "INSERT INTO play_queue (user_id, revision, current_index, position_ms, active_device_id, changed_at, "
        "changed_by) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) ON CONFLICT(user_id) DO UPDATE SET current_index = ?3, "
        "position_ms = ?4, active_device_id = ?5",
        user_id, u.revision, index, position, u.active_device.empty() ? std::optional<std::string>() : u.active_device,
        now_ms(), u.changed_by);
  });
  u.position_dirty = false;
  u.persisted_at = now_ms();
}

int64_t SessionHub::position_now(const UserState& u) const {
  return u.playing ? u.position_ms + std::max<int64_t>(0, now_ms() - u.at_ms) : u.position_ms;
}

std::optional<size_t> SessionHub::index_of(const UserState& u, const std::string& entry_id) const {
  for (size_t i = 0; i < u.queue.size(); ++i)
    if (u.queue[i].entry_id == entry_id) return i;
  return std::nullopt;
}

// ---- messages out ----

json SessionHub::playback_fields(const UserState& u) const {
  json j = {{"revision", u.revision},
            {"currentEntryId", u.current ? json(*u.current) : json(nullptr)},
            {"positionMs", u.position_ms},
            {"playing", u.playing},
            {"activeDeviceId", u.active_device.empty() ? json(nullptr) : json(u.active_device)},
            {"volume", nullptr},
            {"muted", nullptr},
            {"at", u.at_ms}};
  if (auto d = u.devices.find(u.active_device); d != u.devices.end()) {
    j["volume"] = d->second.volume;
    j["muted"] = d->second.muted;
  }
  return j;
}

json SessionHub::state_json(const UserState& u) const {
  json q = json::array();
  for (const auto& e : u.queue) q.push_back({{"entryId", e.entry_id}, {"trackId", e.track_id}});
  json j = {{"type", "state"}, {"queue", q}};
  j.update(playback_fields(u));
  return j;
}

json SessionHub::playback_json(const UserState& u) const {
  json j = {{"type", "playback"}};
  j.update(playback_fields(u));
  return j;
}

json SessionHub::devices_json(const UserState& u) const {
  json list = json::array();
  for (const auto& [id, d] : u.devices)
    list.push_back({{"id", id},
                    {"name", d.name},
                    {"client", d.client},
                    {"connected", d.connections > 0},
                    {"active", id == u.active_device},
                    {"volume", d.volume},
                    {"muted", d.muted}});
  return {{"type", "devices"}, {"devices", list}};
}

void SessionHub::send(const SessionPtr& s, const json& message) {
  if (s && s->sink) s->sink->send(message.dump(-1, ' ', false, json::error_handler_t::replace));
}

void SessionHub::error(const SessionPtr& s, const std::string& message) {
  send(s, {{"type", "error"}, {"message", message}});
}

void SessionHub::broadcast(UserState& u, const json& message) {
  auto text = message.dump(-1, ' ', false, json::error_handler_t::replace);
  std::erase_if(u.sessions, [](const std::weak_ptr<Session>& w) { return w.expired(); });
  for (const auto& w : u.sessions)
    if (auto s = w.lock(); s && s->sink && !s->device_id.empty()) s->sink->send(text);
}

// ---- connections ----

SessionPtr SessionHub::connect(int64_t user_id, std::shared_ptr<Sink> sink) {
  auto s = std::make_shared<Session>();
  s->user_id = user_id;
  s->sink = std::move(sink);
  std::lock_guard lock(mutex_);
  user(user_id).sessions.push_back(s);
  return s;
}

void SessionHub::disconnect(const SessionPtr& s) {
  std::lock_guard lock(mutex_);
  auto it = users_.find(s->user_id);
  if (it == users_.end()) return;
  auto& u = it->second;
  std::erase_if(u.sessions, [&](const std::weak_ptr<Session>& w) {
    auto p = w.lock();
    return !p || p == s;
  });
  if (s->device_id.empty()) return;
  auto d = u.devices.find(s->device_id);
  if (d != u.devices.end() && d->second.connections > 0) --d->second.connections;
  // The active device stays active while away; save where it was.
  if (s->device_id == u.active_device && u.position_dirty) {
    try {
      persist_position(s->user_id, u);
    } catch (const std::exception& e) {
      log::warn("sync: could not save position: {}", e.what());
    }
  }
  broadcast(u, devices_json(u));
  try {
    writer_.run([&](db::Connection& c) {
      c.run("UPDATE device SET last_seen_at = ? WHERE id = ?", now_ms(), s->device_id);
    });
  } catch (...) {
  }
}

void SessionHub::receive(const SessionPtr& s, std::string_view text) {
  json m;
  try {
    m = json::parse(text);
  } catch (const json::exception&) {
    error(s, "Malformed JSON");
    return;
  }
  if (!m.is_object() || !m.contains("type") || !m["type"].is_string()) {
    error(s, "A message needs a string \"type\"");
    return;
  }
  const auto type = m["type"].get<std::string>();
  std::lock_guard lock(mutex_);
  auto& u = user(s->user_id);
  try {
    if (type == "hello") return on_hello(s, u, m);
    if (s->device_id.empty()) return error(s, "Send hello first");
    if (type == "queue.edit") return on_queue_edit(s, u, m);
    if (type == "control") return on_control(s, u, m);
    if (type == "progress") return on_progress(s, u, m);
    if (type == "transfer") return on_transfer(s, u, m);
    error(s, "Unknown message type: " + type);
  } catch (const json::exception& e) {
    error(s, std::string("Invalid message: ") + e.what());
  } catch (const std::exception& e) {
    log::error("sync: {} failed: {}", type, e.what());
    error(s, "Internal error");
  }
}

// ---- messages in ----

void SessionHub::on_hello(const SessionPtr& s, UserState& u, const json& m) {
  auto device = m.value("deviceId", "");
  if (device.empty() || device.size() > 128) return error(s, "hello needs a deviceId (1-128 characters)");
  if (!s->device_id.empty()) return error(s, "hello was already sent");
  auto name = m.value("deviceName", device);
  auto client = m.value("client", "");
  std::optional<int64_t> owner;
  int volume = 100;
  bool muted = false;
  writer_.run([&](db::Connection& c) {
    auto st = c.prepare("SELECT user_id, volume, muted FROM device WHERE id = ?");
    st.bind(1, device);
    if (st.step()) {
      owner = st.int64(0);
      volume = static_cast<int>(st.int64(1));
      muted = st.int64(2) != 0;
    }
  });
  if (owner && *owner != s->user_id) return error(s, "This deviceId belongs to another user");
  writer_.run([&](db::Connection& c) {
    c.run(
        "INSERT INTO device (id, user_id, name, client, last_seen_at) VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET name = excluded.name, client = excluded.client, "
        "last_seen_at = excluded.last_seen_at",
        device, s->user_id, name, client, now_ms());
  });
  s->device_id = device;
  auto& d = u.devices[device];
  d.name = name;
  d.client = client;
  d.volume = volume;  // the device keeps its last volume across restarts
  d.muted = muted;
  ++d.connections;
  send(s, state_json(u));
  broadcast(u, devices_json(u));
}

void SessionHub::on_queue_edit(const SessionPtr& s, UserState& u, const json& m) {
  if (!m.contains("ops") || !m["ops"].is_array()) return error(s, "queue.edit needs an ops array");
  // Only tracks that exist (and are not missing) can enter the queue.
  auto known = [&](const json& tracks) {
    std::vector<std::string> ids;
    if (!tracks.is_array()) return ids;
    for (const auto& t : tracks)
      if (t.is_string()) ids.push_back(t.get<std::string>());
    if (ids.empty()) return ids;
    std::vector<std::string> out;
    writer_.run([&](db::Connection& c) {
      for (const auto& id : ids)
        if (c.scalar_int("SELECT 1 FROM track WHERE id = ? AND missing = 0", id)) out.push_back(id);
    });
    return out;
  };
  auto make = [](const std::vector<std::string>& tracks) {
    std::vector<Entry> v;
    for (const auto& t : tracks) v.push_back({ids::random_id(), t});
    return v;
  };

  bool changed = false;
  for (const auto& op : m["ops"]) {
    auto kind = op.value("op", "");
    if (kind == "replace") {
      u.queue = make(known(op.value("tracks", json::array())));
      auto idx = op.value("currentIndex", int64_t{0});
      u.current = (idx >= 0 && idx < static_cast<int64_t>(u.queue.size()))
                      ? std::optional(u.queue[static_cast<size_t>(idx)].entry_id)
                      : (u.queue.empty() ? std::nullopt : std::optional(u.queue.front().entry_id));
      u.position_ms = op.value("positionMs", int64_t{0});
      u.at_ms = now_ms();
      changed = true;
    } else if (kind == "insert") {
      auto entries = make(known(op.value("tracks", json::array())));
      if (entries.empty()) continue;
      size_t at = u.queue.size();
      if (op.contains("afterEntryId")) {
        auto i = index_of(u, op["afterEntryId"].get<std::string>());
        if (!i) continue;  // the anchor is gone: drop the op
        at = *i + 1;
      } else if (op.contains("index")) {
        at = static_cast<size_t>(std::clamp<int64_t>(op["index"].get<int64_t>(), 0, static_cast<int64_t>(u.queue.size())));
      }
      u.queue.insert(u.queue.begin() + static_cast<long>(at), entries.begin(), entries.end());
      if (!u.current) {
        u.current = entries.front().entry_id;
        u.position_ms = 0;
        u.at_ms = now_ms();
      }
      changed = true;
    } else if (kind == "remove") {
      std::vector<std::string> gone;
      for (const auto& e : op.value("entryIds", json::array())) gone.push_back(e.get<std::string>());
      std::optional<size_t> current_index = u.current ? index_of(u, *u.current) : std::nullopt;
      bool current_removed = u.current && std::find(gone.begin(), gone.end(), *u.current) != gone.end();
      size_t before = u.queue.size();
      size_t removed_before_current = 0;
      for (size_t i = 0; i < u.queue.size() && current_index && i < *current_index; ++i)
        if (std::find(gone.begin(), gone.end(), u.queue[i].entry_id) != gone.end()) ++removed_before_current;
      std::erase_if(u.queue, [&](const Entry& e) { return std::find(gone.begin(), gone.end(), e.entry_id) != gone.end(); });
      if (u.queue.size() == before) continue;
      if (current_removed) {
        // The next entry (now at the same position) becomes current.
        size_t at = *current_index - removed_before_current;
        if (u.queue.empty()) {
          u.current.reset();
          u.playing = false;
        } else {
          u.current = u.queue[std::min(at, u.queue.size() - 1)].entry_id;
        }
        u.position_ms = 0;
        u.at_ms = now_ms();
      }
      changed = true;
    } else if (kind == "move") {
      auto i = index_of(u, op.value("entryId", ""));
      if (!i) continue;
      auto e = u.queue[*i];
      u.queue.erase(u.queue.begin() + static_cast<long>(*i));
      auto to = static_cast<size_t>(std::clamp<int64_t>(op.value("toIndex", int64_t{0}), 0, static_cast<int64_t>(u.queue.size())));
      u.queue.insert(u.queue.begin() + static_cast<long>(to), e);
      changed = true;
    } else if (kind == "clear") {
      u.queue.clear();
      u.current.reset();
      u.playing = false;
      u.position_ms = 0;
      u.at_ms = now_ms();
      changed = true;
    } else {
      error(s, "Unknown queue op: " + kind);
    }
  }
  if (!changed) return;
  ++u.revision;
  u.changed_by = u.devices[s->device_id].name;
  persist_queue(s->user_id, u);
  broadcast(u, state_json(u));
}

void SessionHub::on_control(const SessionPtr& s, UserState& u, const json& m) {
  auto action = m.value("action", "");
  const auto now = now_ms();
  u.position_ms = position_now(u);
  u.at_ms = now;
  auto index = u.current ? index_of(u, *u.current) : std::nullopt;
  if (action == "play") {
    if (!u.current && !u.queue.empty()) {
      u.current = u.queue.front().entry_id;
      u.position_ms = 0;
    }
    u.playing = u.current.has_value();
  } else if (action == "pause") {
    u.playing = false;
  } else if (action == "seek") {
    u.position_ms = std::max<int64_t>(0, m.value("positionMs", int64_t{0}));
  } else if (action == "jump") {
    auto entry = m.value("entryId", "");
    if (!index_of(u, entry)) return error(s, "No such entry: " + entry);
    u.current = entry;
    u.position_ms = 0;
  } else if (action == "next") {
    if (index && *index + 1 < u.queue.size()) {
      u.current = u.queue[*index + 1].entry_id;
    } else {
      u.playing = false;  // end of the queue
    }
    u.position_ms = 0;
  } else if (action == "previous") {
    // Like most players: past the first few seconds, "previous" restarts the song.
    if (u.position_ms <= 3000 && index && *index > 0) u.current = u.queue[*index - 1].entry_id;
    u.position_ms = 0;
  } else if (action == "volume" || action == "mute") {
    // Volume belongs to the device that makes the sound.
    auto d = u.devices.find(u.active_device);
    if (d == u.devices.end()) return error(s, "There is no active device");
    if (action == "volume") {
      if (!m.contains("volume") || !m["volume"].is_number()) return error(s, "volume needs a number 0-100");
      d->second.volume = std::clamp(m["volume"].get<int>(), 0, 100);
    } else {
      d->second.muted = m.value("muted", !d->second.muted);
    }
    persist_volume(d->first, d->second);
    broadcast(u, playback_json(u));
    return;
  } else {
    return error(s, "Unknown control action: " + action);
  }
  u.position_dirty = true;
  if (!u.playing) persist_position(s->user_id, u);
  broadcast(u, playback_json(u));
}

void SessionHub::on_progress(const SessionPtr& s, UserState& u, const json& m) {
  // Only the device that makes the sound reports where it is; the first reporter
  // becomes the active device when none is set.
  if (u.active_device.empty()) u.active_device = s->device_id;
  if (s->device_id != u.active_device) return;
  if (m.contains("entryId") && m["entryId"].is_string()) {
    auto entry = m["entryId"].get<std::string>();
    if (index_of(u, entry)) u.current = entry;
  }
  u.position_ms = std::max<int64_t>(0, m.value("positionMs", u.position_ms));
  // The device reports volume changes made on itself (buttons, hardware knob).
  if (m.contains("volume") || m.contains("muted")) {
    auto& d = u.devices[s->device_id];
    bool changed = false;
    if (m.contains("volume") && m["volume"].is_number()) {
      int v = std::clamp(m["volume"].get<int>(), 0, 100);
      changed = changed || v != d.volume;
      d.volume = v;
    }
    if (m.contains("muted") && m["muted"].is_boolean()) {
      changed = changed || m["muted"].get<bool>() != d.muted;
      d.muted = m["muted"].get<bool>();
    }
    if (changed) persist_volume(s->device_id, d);
  }
  bool was_playing = u.playing;
  u.playing = m.value("playing", u.playing);
  u.at_ms = now_ms();
  u.position_dirty = true;
  if (was_playing && !u.playing) persist_position(s->user_id, u);
  broadcast(u, playback_json(u));
}

void SessionHub::on_transfer(const SessionPtr& s, UserState& u, const json& m) {
  auto target = m.value("targetDeviceId", "");
  auto d = u.devices.find(target);
  if (d == u.devices.end() || d->second.connections == 0) return error(s, "That device is not connected");
  u.position_ms = position_now(u);
  u.at_ms = now_ms();
  u.active_device = target;
  u.position_dirty = true;
  persist_position(s->user_id, u);
  broadcast(u, playback_json(u));
  broadcast(u, devices_json(u));
}

void SessionHub::persist_volume(const std::string& device_id, const Device& d) {
  writer_.run([&](db::Connection& c) {
    c.run("UPDATE device SET volume = ?, muted = ? WHERE id = ?", d.volume, d.muted, device_id);
  });
}

// ---- outside events ----

void SessionHub::queue_replaced(int64_t user_id) {
  std::lock_guard lock(mutex_);
  auto it = users_.find(user_id);
  if (it == users_.end() || !it->second.loaded) return;  // loaded fresh on next use
  auto& u = it->second;
  load(user_id, u);
  u.at_ms = now_ms();
  broadcast(u, state_json(u));
}

void SessionHub::flush(int64_t user_id) {
  std::lock_guard lock(mutex_);
  auto it = users_.find(user_id);
  if (it != users_.end() && it->second.loaded && it->second.position_dirty) persist_position(user_id, it->second);
}

void SessionHub::flush_all() {
  std::lock_guard lock(mutex_);
  for (auto& [id, u] : users_)
    if (u.loaded && u.position_dirty) persist_position(id, u);
}

json SessionHub::state_message(int64_t user_id) {
  std::lock_guard lock(mutex_);
  return state_json(user(user_id));
}

void SessionHub::flusher() {
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    cv_.wait_for(lock, std::min<std::chrono::milliseconds>(flush_every_, std::chrono::seconds(1)));
    if (stopping_) break;
    auto now = now_ms();
    for (auto& [id, u] : users_) {
      if (!u.loaded || !u.position_dirty || now - u.persisted_at < flush_every_.count()) continue;
      try {
        persist_position(id, u);
      } catch (const std::exception& e) {
        log::warn("sync: could not save position for user {}: {}", id, e.what());
      }
    }
  }
}

}  // namespace eos::sync
