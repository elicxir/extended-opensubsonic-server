#include "scanner/scanner.h"

#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/ids.h"
#include "core/log.h"
#include "core/normalize.h"
#include "core/strings.h"
#include "db/database.h"
#include "scanner/tag_reader.h"
#include "scanner/track_info.h"

namespace eos::scanner {
namespace fs = std::filesystem;

namespace {

constexpr int kSourceTag = 2;
constexpr size_t kBatchSize = 200;

struct Existing {
  std::string id, path, pid, album_id;
  std::optional<std::string> md5;
  int64_t size = 0, mtime = 0;
  bool missing = false;
};

struct Found {
  std::string rel;
  int64_t size = 0, mtime = 0;
};

// One file that has to be (re)read.
struct Job {
  Found file;
  const Existing* existing = nullptr;  // set when the path is already known
};

struct Result {
  TrackInfo info;
  std::string id;
  std::string old_album_id;  // empty for brand-new tracks
  std::string album_id;      // decided in the album step
  enum class Kind { added, updated, moved, restored } kind = Kind::added;
};

int64_t mtime_ms_of(const struct stat& st) {
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1'000'000;
}

std::string dir_of(const std::string& rel) {
  auto pos = rel.rfind('/');
  return pos == std::string::npos ? std::string() : rel.substr(0, pos);
}

void upsert_artist(db::Connection& c, const Person& p) {
  auto id = artist_id(p.name);
  c.run(
      "INSERT INTO artist (id, name, sort_name, name_norm, mbz_artist_id) VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
      "sort_name = COALESCE(excluded.sort_name, artist.sort_name), "
      "mbz_artist_id = COALESCE(excluded.mbz_artist_id, artist.mbz_artist_id)",
      id, p.name, p.sort_name.empty() ? std::optional<std::string>() : p.sort_name, normalize(p.name),
      p.mbid.empty() ? std::optional<std::string>() : p.mbid);
}

void insert_participants(db::Connection& c, const char* entity_type, const std::string& entity_id,
                         const char* role, const std::vector<Person>& people) {
  int pos = 0;
  for (const auto& p : people) {
    upsert_artist(c, p);
    c.run(
        "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, "
        "position, source) VALUES (?, ?, ?, ?, ?, ?, ?)",
        entity_type, entity_id, artist_id(p.name), role, p.sub_role, pos++, kSourceTag);
  }
}

// Manual values win over tags (design: 情報の出どころ). Applied before writing.
std::map<std::string, std::string> overrides_for(db::Connection& c, const char* type,
                                                 const std::string& id) {
  std::map<std::string, std::string> out;
  auto st = c.prepare("SELECT field, value FROM field_override WHERE entity_type = ? AND entity_id = ?");
  st.bind_all(type, id);
  while (st.step())
    if (!st.is_null(1)) out[st.text(0)] = st.text(1);
  return out;
}

std::optional<std::string> nonempty(const std::string& s) {
  return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

void write_album(db::Connection& c, int64_t lib_id, const Result& r, int64_t now) {
  const auto& ti = r.info;
  auto ov = overrides_for(c, "album", r.album_id);
  auto name = ov.count("name") ? ov["name"] : ti.album;
  auto sort_name = ov.count("sort_name") ? std::optional(ov["sort_name"]) : nonempty(ti.album_sort);
  c.run(
      "INSERT INTO album (id, pid, library_id, name, sort_name, display_artist, release_date, "
      "original_release_date, year, kind, mbz_album_id, is_compilation, dir_path, created_at, "
      "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'physical', ?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET pid = excluded.pid, name = excluded.name, "
      "sort_name = excluded.sort_name, display_artist = excluded.display_artist, "
      "release_date = excluded.release_date, original_release_date = excluded.original_release_date, "
      "year = excluded.year, mbz_album_id = excluded.mbz_album_id, "
      "is_compilation = excluded.is_compilation, dir_path = excluded.dir_path, "
      "updated_at = excluded.updated_at",
      r.album_id, ti.album_pid, lib_id, name, sort_name, ti.display_album_artist,
      nonempty(ti.release_date), nonempty(ti.original_date), ti.year, nonempty(ti.mbz_album_id),
      ti.compilation, dir_of(ti.rel_path), now, now);
  c.run("DELETE FROM participant WHERE entity_type = 'album' AND entity_id = ? AND source = ?",
        r.album_id, kSourceTag);
  insert_participants(c, "album", r.album_id, "albumartist", ti.album_artists);
}

void write_track(db::Connection& c, int64_t lib_id, const Result& r, int64_t now) {
  const auto& ti = r.info;
  auto ov = overrides_for(c, "track", r.id);
  auto title = ov.count("title") ? ov["title"] : ti.title;
  auto sort_title = ov.count("sort_title") ? std::optional(ov["sort_title"]) : nonempty(ti.sort_title);
  auto subtitle = ov.count("subtitle") ? std::optional(ov["subtitle"]) : nonempty(ti.subtitle);
  nlohmann::json tags_json = ti.raw.tags;

  c.run(
      "INSERT INTO track (id, pid, library_id, path, album_id, title, sort_title, subtitle, "
      "disc_number, track_number, duration_ms, suffix, content_type, bit_rate, sample_rate, "
      "bit_depth, channels, size, mtime, audio_md5, mbz_recording_id, mbz_track_id, display_artist, "
      "display_composer, performance_kind, recorded_date, release_date, year, has_embedded_art, "
      "tags_json, missing, missing_since, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "?, 0, NULL, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET pid = excluded.pid, library_id = excluded.library_id, "
      "path = excluded.path, album_id = excluded.album_id, title = excluded.title, "
      "sort_title = excluded.sort_title, subtitle = excluded.subtitle, "
      "disc_number = excluded.disc_number, track_number = excluded.track_number, "
      "duration_ms = excluded.duration_ms, suffix = excluded.suffix, "
      "content_type = excluded.content_type, bit_rate = excluded.bit_rate, "
      "sample_rate = excluded.sample_rate, bit_depth = excluded.bit_depth, "
      "channels = excluded.channels, size = excluded.size, mtime = excluded.mtime, "
      "audio_md5 = excluded.audio_md5, mbz_recording_id = excluded.mbz_recording_id, "
      "mbz_track_id = excluded.mbz_track_id, display_artist = excluded.display_artist, "
      "display_composer = excluded.display_composer, performance_kind = excluded.performance_kind, "
      "recorded_date = excluded.recorded_date, release_date = excluded.release_date, "
      "year = excluded.year, has_embedded_art = excluded.has_embedded_art, "
      "tags_json = excluded.tags_json, missing = 0, missing_since = NULL, "
      "updated_at = excluded.updated_at",
      r.id, ti.track_pid, lib_id, ti.rel_path, r.album_id, title, sort_title, subtitle, ti.disc,
      ti.track, ti.raw.duration_ms, ti.suffix, ti.content_type,
      ti.raw.bit_rate ? std::optional<int64_t>(ti.raw.bit_rate) : std::nullopt,
      ti.raw.sample_rate ? std::optional<int64_t>(ti.raw.sample_rate) : std::nullopt,
      ti.raw.bit_depth ? std::optional<int64_t>(*ti.raw.bit_depth) : std::nullopt,
      ti.raw.channels ? std::optional<int64_t>(ti.raw.channels) : std::nullopt, ti.size,
      ti.mtime_ms, ti.raw.audio_md5, nonempty(ti.mbz_recording_id), nonempty(ti.mbz_track_id),
      ti.display_artist, nonempty(ti.display_composer), ti.performance_kind,
      nonempty(ti.recorded_date), nonempty(ti.release_date), ti.year, ti.raw.has_picture,
      tags_json.dump(), now, now);

  c.run("DELETE FROM participant WHERE entity_type = 'track' AND entity_id = ? AND source = ?", r.id,
        kSourceTag);
  insert_participants(c, "track", r.id, "artist", ti.artists);
  insert_participants(c, "track", r.id, "composer", ti.composers);
  insert_participants(c, "track", r.id, "lyricist", ti.lyricists);
  insert_participants(c, "track", r.id, "arranger", ti.arrangers);
  insert_participants(c, "track", r.id, "conductor", ti.conductors);
  insert_participants(c, "track", r.id, "performer", ti.performers);

  c.run("DELETE FROM track_genre WHERE track_id = ?", r.id);
  int pos = 0;
  for (const auto& g : ti.genres) {
    auto gid = genre_id(g);
    c.run("INSERT OR IGNORE INTO genre (id, name) VALUES (?, ?)", gid, g);
    c.run("INSERT OR IGNORE INTO track_genre (track_id, genre_id, position) VALUES (?, ?, ?)", r.id,
          gid, pos++);
  }

  if (ti.disc && !ti.disc_subtitle.empty()) {
    c.run("INSERT OR REPLACE INTO album_disc (album_id, disc_number, title) VALUES (?, ?, ?)",
          r.album_id, *ti.disc, ti.disc_subtitle);
  }
}

}  // namespace

Scanner::Scanner(const Config& cfg, db::WriteQueue& writer, std::filesystem::path db_path)
    : cfg_(cfg), writer_(writer), db_path_(std::move(db_path)) {}

ScanStatus Scanner::status() const {
  std::lock_guard lock(status_mutex_);
  auto s = status_;
  s.count = count_.load();
  s.folder_count = folders_.load();
  return s;
}

void Scanner::set_phase(const std::string& phase) {
  std::lock_guard lock(status_mutex_);
  status_.phase = phase;
}

ScanStats Scanner::scan(bool full) {
  std::lock_guard scan_lock(scan_mutex_);
  {
    std::lock_guard lock(status_mutex_);
    status_.scanning = true;
    status_.phase = "starting";
  }
  count_ = 0;
  folders_ = 0;
  auto started = db::now_ms();
  auto run_id = writer_.run([&](db::Connection& c) {
    c.run("INSERT INTO scan_run (kind, started_at) VALUES (?, ?)", full ? "full" : "quick", started);
    return c.last_insert_rowid();
  });

  ScanStats total;
  for (const auto& lib : cfg_.libraries) {
    try {
      auto s = scan_library(lib, full, started);
      total.seen += s.seen;
      total.added += s.added;
      total.updated += s.updated;
      total.moved += s.moved;
      total.restored += s.restored;
      total.missing += s.missing;
      total.unchanged += s.unchanged;
      total.purged += s.purged;
      total.errors.insert(total.errors.end(), s.errors.begin(), s.errors.end());
    } catch (const std::exception& e) {
      log::error("scan: library {} failed: {}", lib.path.string(), e.what());
      total.errors.push_back(std::format("{}: {}", lib.path.string(), e.what()));
    }
  }

  auto finished = db::now_ms();
  writer_.run([&](db::Connection& c) {
    c.run(
        "UPDATE scan_run SET finished_at = ?, added = ?, updated = ?, moved = ?, missing = ?, "
        "errors = ? WHERE id = ?",
        finished, total.added, total.updated + total.restored, total.moved, total.missing,
        static_cast<int64_t>(total.errors.size()), run_id);
  });
  {
    std::lock_guard lock(status_mutex_);
    status_.scanning = false;
    status_.phase = "idle";
    status_.last_scan_ms = finished;
  }
  log::info(
      "scan: {} files, {} added, {} updated, {} moved, {} restored, {} missing, {} purged, {} "
      "errors in {} ms",
      total.seen, total.added, total.updated, total.moved, total.restored, total.missing,
      total.purged, total.errors.size(), finished - started);
  return total;
}

ScanStats Scanner::scan_library(const LibraryConfig& lib, bool full, int64_t run_started) {
  ScanStats stats;
  const auto root = fs::weakly_canonical(lib.path);
  if (!fs::is_directory(root)) throw std::runtime_error("library path is not a directory");

  // -- library row
  auto lib_id = writer_.run([&](db::Connection& c) {
    c.run(
        "INSERT INTO library (name, path) VALUES (?, ?) "
        "ON CONFLICT(path) DO UPDATE SET name = excluded.name",
        lib.name, root.string());
    return *c.scalar_int("SELECT id FROM library WHERE path = ?", root.string());
  });

  // -- what the database knows
  set_phase("loading");
  db::Connection rc(db_path_, db::Connection::Mode::read_only);
  std::vector<Existing> existing;
  {
    auto st = rc.prepare(
        "SELECT id, path, pid, album_id, audio_md5, size, mtime, missing FROM track "
        "WHERE library_id = ?");
    st.bind(1, lib_id);
    while (st.step())
      existing.push_back({st.text(0), st.text(1), st.text(2), st.text(3), st.opt_text(4), st.int64(5),
                          st.int64(6), st.int64(7) != 0});
  }
  std::unordered_map<std::string, const Existing*> by_path;
  for (const auto& e : existing) by_path[e.path] = &e;

  // -- walk the library
  set_phase("walking");
  std::vector<Found> found;
  std::unordered_set<std::string> ext_ok(cfg_.extensions.begin(), cfg_.extensions.end());
  if (!fs::exists(root / ".ndignore")) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        stats.errors.push_back(std::format("walk: {}", ec.message()));
        ec.clear();
        continue;
      }
      const auto& entry = *it;
      auto name = entry.path().filename().string();
      std::error_code tec;
      bool is_dir = entry.is_directory(tec);
      if (!name.empty() && name.front() == '.') {
        if (is_dir) it.disable_recursion_pending();
        continue;
      }
      if (is_dir) {
        ++folders_;
        if (fs::exists(entry.path() / ".ndignore")) it.disable_recursion_pending();
        continue;
      }
      if (!entry.is_regular_file(tec)) continue;
      auto ext = to_lower_ascii(entry.path().extension().string());
      if (ext.size() < 2 || !ext_ok.count(ext.substr(1))) continue;
      struct stat st {};
      if (::stat(entry.path().c_str(), &st) != 0) continue;
      found.push_back({entry.path().lexically_relative(root).generic_string(),
                       static_cast<int64_t>(st.st_size), mtime_ms_of(st)});
    }
  }
  stats.seen = static_cast<int64_t>(found.size());

  // -- decide what to read
  std::vector<Job> jobs;
  std::unordered_set<std::string> found_paths;
  for (const auto& f : found) {
    found_paths.insert(f.rel);
    auto it = by_path.find(f.rel);
    if (it == by_path.end()) {
      jobs.push_back({f, nullptr});
    } else {
      const auto* e = it->second;
      if (!full && !e->missing && e->size == f.size && e->mtime == f.mtime) {
        ++stats.unchanged;
      } else {
        jobs.push_back({f, e});
      }
    }
  }

  // -- read tags in parallel
  set_phase("reading");
  const auto pids = PidSpecs::from(cfg_);
  std::vector<std::optional<TrackInfo>> infos(jobs.size());
  std::vector<std::string> read_errors(jobs.size());
  {
    std::atomic<size_t> next{0};
    auto worker = [&] {
      for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
        const auto& j = jobs[i];
        std::string err;
        auto raw = read_file(root / j.file.rel, err);
        if (raw) {
          infos[i] = interpret(*raw, j.file.rel, j.file.size, j.file.mtime, cfg_, pids);
        } else {
          read_errors[i] = std::format("{}: {}", j.file.rel, err);
        }
        ++count_;
      }
    };
    std::vector<std::thread> threads;
    int n = std::max(1, std::min<int>(cfg_.scan_workers, static_cast<int>(jobs.size())));
    for (int t = 0; t < n; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
  }
  for (const auto& e : read_errors)
    if (!e.empty()) {
      log::warn("scan: {}", e);
      stats.errors.push_back(e);
    }

  // -- identify tracks
  set_phase("matching");
  std::vector<Result> results;
  std::vector<size_t> unknown;  // indices into results of files at new paths
  for (size_t i = 0; i < jobs.size(); ++i) {
    if (!infos[i]) continue;
    Result r;
    r.info = std::move(*infos[i]);
    if (jobs[i].existing) {
      r.id = jobs[i].existing->id;
      r.old_album_id = jobs[i].existing->album_id;
      r.kind = jobs[i].existing->missing ? Result::Kind::restored : Result::Kind::updated;
    } else {
      unknown.push_back(results.size());
    }
    results.push_back(std::move(r));
  }

  // Candidates for re-identification: tracks whose path is gone (vanished now, or
  // already missing from an earlier scan).
  std::vector<const Existing*> candidates;
  for (const auto& e : existing)
    if (!found_paths.count(e.path)) candidates.push_back(&e);
  std::vector<bool> claimed(candidates.size(), false);
  std::unordered_map<std::string, std::vector<size_t>> cand_by_pid, cand_by_md5;
  for (size_t i = 0; i < candidates.size(); ++i) {
    cand_by_pid[candidates[i]->pid].push_back(i);
    if (candidates[i]->md5) cand_by_md5[*candidates[i]->md5].push_back(i);
  }
  auto unique_unclaimed = [&](const std::vector<size_t>& list) -> std::optional<size_t> {
    std::optional<size_t> hit;
    for (auto idx : list) {
      if (claimed[idx]) continue;
      if (hit) return std::nullopt;  // ambiguous
      hit = idx;
    }
    return hit;
  };
  auto claim = [&](Result& r, size_t idx) {
    claimed[idx] = true;
    r.id = candidates[idx]->id;
    r.old_album_id = candidates[idx]->album_id;
    r.kind = Result::Kind::moved;
  };
  // Pass 1: same PID. Pass 2: same audio MD5 (tags were edited and the file moved).
  for (auto ri : unknown) {
    auto& r = results[ri];
    auto it = cand_by_pid.find(r.info.track_pid);
    if (it == cand_by_pid.end()) continue;
    if (auto idx = unique_unclaimed(it->second)) claim(r, *idx);
  }
  for (auto ri : unknown) {
    auto& r = results[ri];
    if (!r.id.empty() || !r.info.raw.audio_md5) continue;
    auto it = cand_by_md5.find(*r.info.raw.audio_md5);
    if (it == cand_by_md5.end()) continue;
    if (auto idx = unique_unclaimed(it->second)) claim(r, *idx);
  }
  {
    std::unordered_set<std::string> used_ids;
    auto st = rc.prepare("SELECT id FROM track");
    while (st.step()) used_ids.insert(st.text(0));
    for (auto ri : unknown) {
      auto& r = results[ri];
      if (!r.id.empty()) continue;
      r.id = used_ids.count(r.info.track_pid) ? ids::random_id() : r.info.track_pid;
      used_ids.insert(r.id);
      r.kind = Result::Kind::added;
    }
  }

  // -- decide albums
  std::unordered_map<std::string, std::string> album_by_pid, pid_by_album;
  std::unordered_set<std::string> used_album_ids;
  {
    auto st = rc.prepare("SELECT id, pid, library_id, kind FROM album");
    while (st.step()) {
      used_album_ids.insert(st.text(0));
      if (st.int64(2) == lib_id && st.text(3) == "physical") {
        album_by_pid.emplace(st.text(1), st.text(0));
        pid_by_album[st.text(0)] = st.text(1);
      }
    }
  }
  std::unordered_map<std::string, int64_t> live_count;  // album -> live tracks before this scan
  for (const auto& e : existing)
    if (!e.missing) ++live_count[e.album_id];
  std::unordered_map<std::string, int64_t> leaving;  // album -> tracks leaving it in this scan
  for (const auto& r : results) {
    if (r.old_album_id.empty()) continue;
    auto it = pid_by_album.find(r.old_album_id);
    if (it == pid_by_album.end() || it->second != r.info.album_pid) ++leaving[r.old_album_id];
  }
  for (size_t i = 0; i < candidates.size(); ++i)
    if (!claimed[i] && !candidates[i]->missing) ++leaving[candidates[i]->album_id];

  std::map<std::string, std::vector<size_t>> unresolved;  // new album pid -> results
  for (size_t i = 0; i < results.size(); ++i) {
    auto it = album_by_pid.find(results[i].info.album_pid);
    if (it != album_by_pid.end()) {
      results[i].album_id = it->second;
    } else {
      unresolved[results[i].info.album_pid].push_back(i);
    }
  }
  std::unordered_set<std::string> reused;
  for (auto& [pid, members] : unresolved) {
    // Inherit the old album's id when every live track of that album moves here
    // (the album's tags were edited), so manual data on the album survives.
    std::map<std::string, int64_t> from;
    for (auto i : members)
      if (!results[i].old_album_id.empty()) ++from[results[i].old_album_id];
    std::string chosen;
    for (const auto& [old, n] : from) {
      if (reused.count(old)) continue;
      if (n == live_count[old] && n == leaving[old]) {
        chosen = old;
        break;
      }
    }
    if (chosen.empty()) chosen = used_album_ids.count(pid) ? ids::random_id() : pid;
    reused.insert(chosen);
    used_album_ids.insert(chosen);
    album_by_pid[pid] = chosen;
    for (auto i : members) results[i].album_id = chosen;
  }

  // -- write
  set_phase("writing");
  const auto now = db::now_ms();
  std::vector<std::string> moved_ids;
  for (const auto& r : results)
    if (r.kind == Result::Kind::moved) moved_ids.push_back(r.id);
  if (!moved_ids.empty()) {
    // Park moved tracks on unique placeholder paths so that path swaps inside one scan
    // never collide with the UNIQUE (library_id, path) constraint.
    writer_.run([&](db::Connection& c) {
      db::Transaction tx(c);
      for (const auto& id : moved_ids)
        c.run("UPDATE track SET path = '//moving/' || id WHERE id = ?", id);
      tx.commit();
    });
  }
  std::unordered_set<std::string> albums_written;
  for (size_t start = 0; start < results.size(); start += kBatchSize) {
    size_t end = std::min(results.size(), start + kBatchSize);
    writer_.run([&](db::Connection& c) {
      db::Transaction tx(c);
      for (size_t i = start; i < end; ++i) {
        const auto& r = results[i];
        if (albums_written.insert(r.album_id).second) write_album(c, lib_id, r, now);
        write_track(c, lib_id, r, now);
      }
      tx.commit();
    });
  }
  for (const auto& r : results) {
    switch (r.kind) {
      case Result::Kind::added: ++stats.added; break;
      case Result::Kind::updated: ++stats.updated; break;
      case Result::Kind::moved: ++stats.moved; break;
      case Result::Kind::restored: ++stats.restored; break;
    }
  }

  // -- missing, aggregates, purge
  set_phase("finishing");
  std::set<std::string> touched_albums;
  for (const auto& r : results) {
    touched_albums.insert(r.album_id);
    if (!r.old_album_id.empty()) touched_albums.insert(r.old_album_id);
  }
  std::vector<std::string> newly_missing;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (claimed[i] || candidates[i]->missing) continue;
    newly_missing.push_back(candidates[i]->id);
    touched_albums.insert(candidates[i]->album_id);
  }
  stats.missing = static_cast<int64_t>(newly_missing.size());
  const int64_t purge_before =
      now - static_cast<int64_t>(cfg_.purge_missing_after_days) * 86'400'000LL;
  stats.purged = writer_.run([&](db::Connection& c) {
    db::Transaction tx(c);
    for (const auto& id : newly_missing)
      c.run("UPDATE track SET missing = 1, missing_since = ?, updated_at = ? WHERE id = ?", now, now, id);
    for (const auto& a : touched_albums) {
      c.run(
          "UPDATE album SET "
          "song_count = (SELECT COUNT(*) FROM track WHERE album_id = ?1 AND missing = 0), "
          "duration_ms = (SELECT COALESCE(SUM(duration_ms), 0) FROM track "
          "               WHERE album_id = ?1 AND missing = 0), "
          "missing = NOT EXISTS (SELECT 1 FROM track WHERE album_id = ?1 AND missing = 0), "
          "updated_at = ?2 WHERE id = ?1",
          a, now);
    }
    c.run("DELETE FROM track WHERE library_id = ? AND missing = 1 AND missing_since < ?", lib_id,
          purge_before);
    auto purged = c.changes();
    c.exec(
        "DELETE FROM participant WHERE entity_type = 'track' "
        "  AND entity_id NOT IN (SELECT id FROM track);"
        "DELETE FROM album WHERE kind = 'physical' AND id NOT IN (SELECT album_id FROM track);"
        "DELETE FROM participant WHERE entity_type = 'album' "
        "  AND entity_id NOT IN (SELECT id FROM album);"
        "DELETE FROM artist WHERE id NOT IN (SELECT artist_id FROM participant);"
        "DELETE FROM genre WHERE id NOT IN (SELECT genre_id FROM track_genre);");
    c.run("UPDATE library SET last_scan_at = ? WHERE id = ?", run_started, lib_id);
    tx.commit();
    return purged;
  });
  return stats;
}

}  // namespace eos::scanner
