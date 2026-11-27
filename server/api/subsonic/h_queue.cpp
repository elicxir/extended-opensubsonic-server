// getPlayQueue, savePlayQueue, getPlayQueueByIndex, savePlayQueueByIndex.
// Both forms read and write the same per-user queue that /ext/session (M5) shares.
#include "api/subsonic/builders.h"
#include "api/subsonic/router.h"
#include "core/ids.h"

namespace eos::api {
namespace {

struct Queue {
  std::vector<std::string> tracks;
  std::optional<int64_t> current_index;
  int64_t position_ms = 0;
  int64_t changed = 0;
  std::string changed_by;
};

std::optional<Queue> load_queue(db::Connection& db, int64_t user) {
  auto st = db.prepare(
      "SELECT current_index, position_ms, changed_at, COALESCE(changed_by, '') FROM play_queue WHERE user_id = ?");
  st.bind(1, user);
  if (!st.step()) return std::nullopt;
  Queue q{{}, st.opt_int64(0), st.int64(1), st.int64(2), st.text(3)};
  auto e = db.prepare("SELECT track_id FROM play_queue_entry WHERE user_id = ? ORDER BY position");
  e.bind(1, user);
  while (e.step()) q.tracks.push_back(e.text(0));
  if (q.tracks.empty()) return std::nullopt;
  return q;
}

void save_queue(Ctx& c, const std::vector<std::string>& tracks, std::optional<int64_t> current_index,
                int64_t position_ms) {
  for (const auto& id : tracks)
    if (!c.db.scalar_int("SELECT 1 FROM track WHERE id = ?", id))
      throw SubsonicError(err::not_found, "Song not found: " + id);
  c.write([&](db::Connection& w) {
    db::Transaction tx(w);
    auto now = db::now_ms();
    w.run(
        "INSERT INTO play_queue (user_id, revision, current_index, position_ms, changed_at, changed_by) "
        "VALUES (?1, 1, ?2, ?3, ?4, ?5) ON CONFLICT(user_id) DO UPDATE SET revision = revision + 1, "
        "current_index = ?2, position_ms = ?3, changed_at = ?4, changed_by = ?5",
        c.user.id, current_index, position_ms, now, c.client);
    w.run("DELETE FROM play_queue_entry WHERE user_id = ?", c.user.id);
    int64_t pos = 0;
    for (const auto& t : tracks)
      w.run("INSERT INTO play_queue_entry (user_id, position, entry_id, track_id) VALUES (?, ?, ?, ?)",
            c.user.id, pos++, ids::random_id(), t);
    tx.commit();
  });
}

}  // namespace

void add_queue_handlers(HandlerTable& t) {
  t.add("getPlayQueue", [](Ctx& c, Node& root) {
    auto q = load_queue(c.db, c.user.id);
    if (!q) return;
    Loader loader(c.db, c.user.id);
    Node n("playQueue");
    if (q->current_index && *q->current_index < static_cast<int64_t>(q->tracks.size()))
      n.set("current", q->tracks[static_cast<size_t>(*q->current_index)]);
    n.set("position", q->position_ms)
        .set("username", c.user.username)
        .set("changed", iso_time(q->changed))
        .set("changedBy", q->changed_by);
    n.list("entry", loader.songs(q->tracks, "entry"));
    root.child(std::move(n));
  });

  t.add("savePlayQueue", [](Ctx& c, Node&) {
    auto tracks = c.params("id");
    auto position = c.opt_int("position").value_or(0);
    std::optional<int64_t> index;
    if (auto current = c.param("current"); current && !tracks.empty()) {
      // With duplicates, pick the occurrence closest to the previous index.
      auto prev = load_queue(c.db, c.user.id);
      int64_t prev_index = prev && prev->current_index ? *prev->current_index : 0;
      for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i] != *current) continue;
        auto d = std::llabs(static_cast<int64_t>(i) - prev_index);
        if (!index || d < std::llabs(*index - prev_index)) index = static_cast<int64_t>(i);
      }
      if (!index) throw SubsonicError(err::generic, "current is not in the queue");
    } else if (!tracks.empty()) {
      index = 0;
    }
    save_queue(c, tracks, index, position);
  });

  t.add("getPlayQueueByIndex", [](Ctx& c, Node& root) {
    auto q = load_queue(c.db, c.user.id);
    if (!q) return;
    Loader loader(c.db, c.user.id);
    Node n("playQueueByIndex");
    n.set_opt("currentIndex", q->current_index);
    n.set("position", q->position_ms)
        .set("username", c.user.username)
        .set("changed", iso_time(q->changed))
        .set("changedBy", q->changed_by);
    n.list("entry", loader.songs(q->tracks, "entry"));
    root.child(std::move(n));
  });

  t.add("savePlayQueueByIndex", [](Ctx& c, Node&) {
    auto tracks = c.params("id");
    if (tracks.empty()) {  // no parameters: clear the queue
      save_queue(c, {}, std::nullopt, 0);
      return;
    }
    auto index = c.opt_int("currentIndex");
    if (!index) throw SubsonicError(err::missing_param, "Required parameter is missing: currentIndex");
    if (*index < 0 || *index >= static_cast<int64_t>(tracks.size()))
      throw SubsonicError(err::missing_param, "currentIndex is out of range");
    save_queue(c, tracks, index, c.opt_int("position").value_or(0));
  });
}

}  // namespace eos::api
