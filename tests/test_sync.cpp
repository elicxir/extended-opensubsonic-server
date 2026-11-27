// M5: the shared remote control (SessionHub) with fake devices, its link to the REST
// play queue, and ReplayGain in song data.
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

#include "api/subsonic/router.h"
#include "auth/users.h"
#include "core/config.h"
#include "db/migrate.h"
#include "fixtures.h"
#include "scanner/scanner.h"
#include "sync/hub.h"

using namespace eos;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

class FakeDevice : public sync::Sink {
 public:
  void send(const std::string& text) override {
    std::lock_guard lock(mutex_);
    messages_.push_back(json::parse(text));
  }
  // The last message of a type (null if none), and clears the inbox.
  json last(const std::string& type) {
    std::lock_guard lock(mutex_);
    json found;
    for (const auto& m : messages_)
      if (m["type"] == type) found = m;
    return found;
  }
  int count(const std::string& type) {
    std::lock_guard lock(mutex_);
    int n = 0;
    for (const auto& m : messages_) n += m["type"] == type;
    return n;
  }
  void clear() {
    std::lock_guard lock(mutex_);
    messages_.clear();
  }

 private:
  std::mutex mutex_;
  std::vector<json> messages_;
};

class SyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!test::have_flac_encoder()) GTEST_SKIP() << "flac encoder not installed";
    music = dir.path() / "music";
    cfg.data_dir = dir.path() / "data";
    cfg.libraries = {{"Music", music}};
    cfg.tags = default_custom_tags();
    fs::create_directories(cfg.data_dir);
    for (int i = 1; i <= 4; ++i)
      test::make_flac(music / std::format("{:02}.flac", i), 400 + 10 * i,
                      {{"TITLE", {std::format("Song {}", i)}}, {"ALBUM", {"A"}}, {"ARTIST", {"X"}},
                       {"TRACKNUMBER", {std::to_string(i)}}, {"REPLAYGAIN_TRACK_GAIN", {std::format("-{}.50 dB", i)}},
                       {"REPLAYGAIN_TRACK_PEAK", {"0.98"}}, {"REPLAYGAIN_ALBUM_GAIN", {"-3.25 dB"}}});
    {
      db::Connection c(cfg.db_path());
      db::migrate(c);
      key = crypto::random_bytes(32);
      alice = auth::create_user(c, "alice", "pw", true, key).id;
      bob = auth::create_user(c, "bob", "pw", false, key).id;
      alice_key = auth::create_api_key(c, alice, "test").key;
    }
    writer = std::make_unique<db::WriteQueue>(cfg.db_path());
    scanner = std::make_unique<scanner::Scanner>(cfg, *writer, cfg.db_path());
    scanner->scan(false);
    conn = std::make_unique<db::Connection>(cfg.db_path());
    for (int i = 1; i <= 4; ++i)
      t.push_back(*conn->scalar_text("SELECT id FROM track WHERE path = ?", std::format("{:02}.flac", i)));
    hub = std::make_unique<sync::SessionHub>(*writer, std::chrono::milliseconds(200));
  }

  struct Dev {
    std::shared_ptr<FakeDevice> sink;
    sync::SessionPtr session;
  };
  Dev device(const std::string& id, int64_t user = 0) {
    Dev d{std::make_shared<FakeDevice>(), nullptr};
    d.session = hub->connect(user ? user : alice, d.sink);
    hub->receive(d.session, json{{"type", "hello"}, {"deviceId", id}, {"deviceName", id + " name"}}.dump());
    return d;
  }
  void send(Dev& d, const json& m) { hub->receive(d.session, m.dump()); }
  std::vector<std::string> tracks_of(const json& state) {
    std::vector<std::string> v;
    for (const auto& e : state["queue"]) v.push_back(e["trackId"]);
    return v;
  }

  test::TempDir dir;
  fs::path music;
  Config cfg;
  crypto::Bytes key;
  int64_t alice = 0, bob = 0;
  std::string alice_key;
  std::vector<std::string> t;
  std::unique_ptr<db::WriteQueue> writer;
  std::unique_ptr<scanner::Scanner> scanner;
  std::unique_ptr<db::Connection> conn;
  std::unique_ptr<sync::SessionHub> hub;
};

}  // namespace

TEST_F(SyncTest, HelloFirstThenState) {
  Dev d{std::make_shared<FakeDevice>(), nullptr};
  d.session = hub->connect(alice, d.sink);
  send(d, {{"type", "control"}, {"action", "play"}});
  EXPECT_EQ(d.sink->last("error")["message"], "Send hello first");
  send(d, {{"type", "hello"}});
  EXPECT_TRUE(d.sink->last("error")["message"].get<std::string>().starts_with("hello needs"));
  hub->receive(d.session, "not json");
  EXPECT_EQ(d.sink->last("error")["message"], "Malformed JSON");

  send(d, {{"type", "hello"}, {"deviceId", "pi"}, {"deviceName", "Pi"}});
  auto state = d.sink->last("state");
  EXPECT_TRUE(state["queue"].empty());
  EXPECT_EQ(state["activeDeviceId"], nullptr);
  auto devices = d.sink->last("devices")["devices"];
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices[0]["name"], "Pi");
  EXPECT_EQ(devices[0]["connected"], true);
  EXPECT_EQ(conn->scalar_text("SELECT name FROM device WHERE id = 'pi'"), "Pi");

  // A device id belongs to one user.
  auto other = device("pi", bob);
  EXPECT_EQ(other.sink->last("error")["message"], "This deviceId belongs to another user");
}

TEST_F(SyncTest, QueueEditsInArrivalOrder) {
  auto a = device("a"), b = device("b");
  send(a, {{"type", "queue.edit"}, {"ops", {{{"op", "replace"}, {"tracks", {t[0], t[1], "nope"}}}}}});
  auto s = b.sink->last("state");  // everyone gets the whole queue
  EXPECT_EQ(s["revision"], 1);
  EXPECT_EQ(tracks_of(s), (std::vector<std::string>{t[0], t[1]}));  // unknown tracks dropped
  std::string e0 = s["queue"][0]["entryId"], e1 = s["queue"][1]["entryId"];
  EXPECT_EQ(s["currentEntryId"], e0);

  // b inserts after the first entry, adds a duplicate at the end, then moves it to the front.
  send(b, {{"type", "queue.edit"},
           {"ops", {{{"op", "insert"}, {"afterEntryId", e0}, {"tracks", {t[2]}}},
                    {{"op", "insert"}, {"tracks", {t[0]}}}}}});
  s = a.sink->last("state");
  EXPECT_EQ(s["revision"], 2);
  EXPECT_EQ(tracks_of(s), (std::vector<std::string>{t[0], t[2], t[1], t[0]}));
  std::string dup = s["queue"][3]["entryId"];
  send(a, {{"type", "queue.edit"}, {"ops", {{{"op", "move"}, {"entryId", dup}, {"toIndex", 0}}}}});
  s = a.sink->last("state");
  EXPECT_EQ(tracks_of(s), (std::vector<std::string>{t[0], t[0], t[2], t[1]}));
  EXPECT_EQ(s["currentEntryId"], e0);  // entry ids keep duplicates apart

  // Removing the current entry makes the next one current; stale ids are ignored.
  send(b, {{"type", "queue.edit"}, {"ops", {{{"op", "remove"}, {"entryIds", {e0, "gone"}}}}}});
  s = a.sink->last("state");
  EXPECT_EQ(tracks_of(s), (std::vector<std::string>{t[0], t[2], t[1]}));
  EXPECT_EQ(s["queue"][1]["trackId"], t[2]);
  EXPECT_EQ(s["currentEntryId"], s["queue"][1]["entryId"]);
  // An op with a vanished anchor is dropped, and a message with no change is not broadcast.
  a.sink->clear();
  send(b, {{"type", "queue.edit"}, {"ops", {{{"op", "insert"}, {"afterEntryId", "gone"}, {"tracks", {t[3]}}}}}});
  EXPECT_EQ(a.sink->count("state"), 0);

  // Saved before it was sent.
  EXPECT_EQ(conn->scalar_int("SELECT revision FROM play_queue WHERE user_id = ?", alice), 4);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM play_queue_entry WHERE user_id = ?", alice), 3);
  EXPECT_EQ(conn->scalar_int("SELECT current_index FROM play_queue WHERE user_id = ?", alice), 1);
  EXPECT_EQ(conn->scalar_text("SELECT changed_by FROM play_queue WHERE user_id = ?", alice), "b name");

  send(a, {{"type", "queue.edit"}, {"ops", {{{"op", "clear"}}}}});
  s = b.sink->last("state");
  EXPECT_TRUE(s["queue"].empty());
  EXPECT_EQ(s["currentEntryId"], nullptr);
}

TEST_F(SyncTest, ControlProgressAndTransfer) {
  auto pi = device("pi"), phone = device("phone");
  send(phone, {{"type", "queue.edit"}, {"ops", {{{"op", "replace"}, {"tracks", {t[0], t[1], t[2]}}}}}});
  auto q = phone.sink->last("state")["queue"];

  // The first device to report progress becomes the active one; others are ignored.
  send(pi, {{"type", "progress"}, {"entryId", q[0]["entryId"]}, {"positionMs", 1000}, {"playing", true}});
  auto p = phone.sink->last("playback");
  EXPECT_EQ(p["activeDeviceId"], "pi");
  EXPECT_EQ(p["positionMs"], 1000);
  EXPECT_EQ(p["playing"], true);
  send(phone, {{"type", "progress"}, {"positionMs", 99999}, {"playing", false}});
  EXPECT_EQ(pi.sink->last("playback")["positionMs"], 1000);

  // Remote commands from the phone.
  send(phone, {{"type", "control"}, {"action", "next"}});
  p = pi.sink->last("playback");
  EXPECT_EQ(p["currentEntryId"], q[1]["entryId"]);
  EXPECT_EQ(p["positionMs"], 0);
  send(phone, {{"type", "control"}, {"action", "seek"}, {"positionMs", 60000}});
  send(phone, {{"type", "control"}, {"action", "previous"}});  // past 3 s: restart the song
  EXPECT_EQ(pi.sink->last("playback")["currentEntryId"], q[1]["entryId"]);
  send(phone, {{"type", "control"}, {"action", "previous"}});  // at the start: the song before
  EXPECT_EQ(pi.sink->last("playback")["currentEntryId"], q[0]["entryId"]);
  send(phone, {{"type", "control"}, {"action", "jump"}, {"entryId", q[2]["entryId"]}});
  send(phone, {{"type", "control"}, {"action", "next"}});  // past the end: stop
  EXPECT_EQ(pi.sink->last("playback")["playing"], false);
  send(phone, {{"type", "control"}, {"action", "jump"}, {"entryId", "nope"}});
  EXPECT_EQ(phone.sink->last("error")["message"], "No such entry: nope");
  send(phone, {{"type", "control"}, {"action", "pause"}});
  EXPECT_EQ(conn->scalar_int("SELECT current_index FROM play_queue WHERE user_id = ?", alice), 2);

  // Hand playback to the phone.
  send(pi, {{"type", "control"}, {"action", "jump"}, {"entryId", q[1]["entryId"]}});
  send(pi, {{"type", "control"}, {"action", "seek"}, {"positionMs", 42000}});
  send(pi, {{"type", "transfer"}, {"targetDeviceId", "phone"}});
  p = pi.sink->last("playback");
  EXPECT_EQ(p["activeDeviceId"], "phone");
  EXPECT_EQ(p["positionMs"], 42000);
  EXPECT_EQ(conn->scalar_text("SELECT active_device_id FROM play_queue WHERE user_id = ?", alice), "phone");
  send(pi, {{"type", "transfer"}, {"targetDeviceId", "tv"}});
  EXPECT_EQ(pi.sink->last("error")["message"], "That device is not connected");
}

TEST_F(SyncTest, VolumeIsPerDeviceAndRemembered) {
  auto pi = device("pi"), phone = device("phone");
  send(phone, {{"type", "control"}, {"action", "volume"}, {"volume", 30}});
  EXPECT_EQ(phone.sink->last("error")["message"], "There is no active device");
  send(pi, {{"type", "progress"}, {"positionMs", 0}, {"playing", true}});  // pi becomes active

  send(phone, {{"type", "control"}, {"action", "volume"}, {"volume", 140}});
  auto p = pi.sink->last("playback");
  EXPECT_EQ(p["volume"], 100);  // clamped
  send(phone, {{"type", "control"}, {"action", "volume"}, {"volume", 35}});
  send(phone, {{"type", "control"}, {"action", "mute"}, {"muted", true}});
  p = pi.sink->last("playback");
  EXPECT_EQ(p["volume"], 35);
  EXPECT_EQ(p["muted"], true);
  // A change made on the device itself arrives with its progress.
  send(pi, {{"type", "progress"}, {"positionMs", 5000}, {"playing", true}, {"volume", 40}, {"muted", false}});
  EXPECT_EQ(phone.sink->last("playback")["volume"], 40);
  EXPECT_EQ(conn->scalar_int("SELECT volume FROM device WHERE id = 'pi'"), 40);

  // After a transfer the phone's own volume applies.
  send(pi, {{"type", "transfer"}, {"targetDeviceId", "phone"}});
  EXPECT_EQ(pi.sink->last("playback")["volume"], 100);
  // The pi gets its 40 back after reconnecting.
  hub->disconnect(pi.session);
  auto pi2 = device("pi");
  for (const auto& d : pi2.sink->last("devices")["devices"])
    if (d["id"] == "pi") EXPECT_EQ(d["volume"], 40);
}

TEST_F(SyncTest, DisconnectReconnectAndRestart) {
  auto pi = device("pi"), phone = device("phone");
  send(phone, {{"type", "queue.edit"}, {"ops", {{{"op", "replace"}, {"tracks", {t[0], t[1]}}, {"currentIndex", 1}}}}});
  send(pi, {{"type", "progress"}, {"positionMs", 12000}, {"playing", true}});
  hub->disconnect(pi.session);
  auto devices = phone.sink->last("devices")["devices"];
  for (const auto& d : devices)
    if (d["id"] == "pi") {
      EXPECT_EQ(d["connected"], false);
      EXPECT_EQ(d["active"], true);  // still the active device while away
    }
  // The position was saved when the active device left.
  EXPECT_GE(conn->scalar_int("SELECT position_ms FROM play_queue WHERE user_id = ?", alice).value_or(0), 12000);

  auto back = device("pi");
  auto s = back.sink->last("state");
  EXPECT_EQ(tracks_of(s), (std::vector<std::string>{t[0], t[1]}));
  EXPECT_EQ(s["activeDeviceId"], "pi");
  send(back, {{"type", "progress"}, {"positionMs", 15000}, {"playing", false}});

  // A new hub (server restart) starts from what was saved.
  hub.reset();
  hub = std::make_unique<sync::SessionHub>(*writer, std::chrono::milliseconds(200));
  auto s2 = hub->state_message(alice);
  EXPECT_EQ(tracks_of(s2), (std::vector<std::string>{t[0], t[1]}));
  EXPECT_EQ(s2["currentEntryId"], s2["queue"][1]["entryId"]);
  EXPECT_EQ(s2["positionMs"], 15000);
  EXPECT_EQ(s2["activeDeviceId"], "pi");
}

TEST_F(SyncTest, PositionIsSavedWhilePlaying) {
  auto pi = device("pi");
  send(pi, {{"type", "queue.edit"}, {"ops", {{{"op", "replace"}, {"tracks", {t[0]}}}}}});
  send(pi, {{"type", "progress"}, {"positionMs", 30000}, {"playing", true}});
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // flush_every is 200 ms here
  EXPECT_GE(conn->scalar_int("SELECT position_ms FROM play_queue WHERE user_id = ?", alice).value_or(0), 30000);
}

TEST_F(SyncTest, RestPlayQueueSharesTheSameQueue) {
  db::ReadPool reads(cfg.db_path(), 2);
  api::Services services(cfg, reads, *writer, *scanner, key);
  services.hub = hub.get();
  api::Router router(services);
  auto call = [&](const std::string& ep, std::vector<std::pair<std::string, std::string>> p) {
    api::Request r;
    r.path = "/rest/" + ep;
    p.emplace_back("apiKey", alice_key);
    p.emplace_back("f", "json");
    r.params = std::move(p);
    r.peer_ip = "127.0.0.1";
    return json::parse(router.handle(r).body)["subsonic-response"];
  };

  auto pi = device("pi");
  // An existing client (Feishin, say) saves a queue: devices see it at once.
  call("savePlayQueue", {{"id", t[2]}, {"id", t[3]}, {"current", t[3]}, {"position", "7000"}, {"c", "feishin"}});
  auto s = pi.sink->last("state");
  EXPECT_EQ(tracks_of(s), (std::vector<std::string>{t[2], t[3]}));
  EXPECT_EQ(s["currentEntryId"], s["queue"][1]["entryId"]);
  EXPECT_EQ(s["positionMs"], 7000);

  // ...and reads the live position the active device reported.
  send(pi, {{"type", "progress"}, {"positionMs", 9000}, {"playing", true}});
  auto q = call("getPlayQueue", {})["playQueue"];
  EXPECT_GE(q["position"].get<int64_t>(), 9000);
  EXPECT_EQ(q["current"], t[3]);
}

TEST_F(SyncTest, ReplayGainInSongs) {
  db::ReadPool reads(cfg.db_path(), 1);
  api::Services services(cfg, reads, *writer, *scanner, key);
  api::Router router(services);
  api::Request r;
  r.path = "/rest/getSong";
  r.params = {{"apiKey", alice_key}, {"f", "json"}, {"id", t[1]}};
  r.peer_ip = "127.0.0.1";
  auto rg = json::parse(router.handle(r).body)["subsonic-response"]["song"]["replayGain"];
  EXPECT_DOUBLE_EQ(rg["trackGain"].get<double>(), -2.5);
  EXPECT_DOUBLE_EQ(rg["trackPeak"].get<double>(), 0.98);
  EXPECT_DOUBLE_EQ(rg["albumGain"].get<double>(), -3.25);
  EXPECT_FALSE(rg.contains("albumPeak"));

  // Opus-style R128 gains are converted to the ReplayGain reference (-18 LUFS).
  test::retag_flac(music / "01.flac", {{"TITLE", {"Song 1"}}, {"ALBUM", {"A"}}, {"ARTIST", {"X"}},
                                       {"TRACKNUMBER", {"1"}}, {"R128_TRACK_GAIN", {"-1792"}}});
  scanner->scan(false);
  r.params = {{"apiKey", alice_key}, {"f", "json"}, {"id", t[0]}};
  rg = json::parse(router.handle(r).body)["subsonic-response"]["song"]["replayGain"];
  EXPECT_DOUBLE_EQ(rg["trackGain"].get<double>(), -2.0);  // -1792 / 256 + 5
}
