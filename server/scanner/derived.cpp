#include "scanner/derived.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "core/ids.h"
#include "core/normalize.h"
#include "core/strings.h"

namespace eos::scanner {
namespace {

std::optional<std::string> nonempty(const std::string& s) {
  return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

std::vector<std::string> sorted_norm_names(const std::vector<Person>& people) {
  std::vector<std::string> out;
  for (const auto& p : people) out.push_back(normalize(p.name));
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

const std::vector<std::string>& custom(const TrackInfo& ti, const char* name) {
  static const std::vector<std::string> none;
  auto it = ti.custom.find(name);
  return it == ti.custom.end() ? none : it->second;
}

// Finds an existing row by a unique key, or inserts one with `id`. Returns the id in use.
std::string title_for(db::Connection& c, const std::string& name, int64_t now) {
  auto norm = normalize(name);
  if (norm.empty()) return {};
  if (auto id = c.scalar_text("SELECT id FROM title WHERE name_norm = ?", norm)) return *id;
  auto id = title_key_id(name);
  c.run("INSERT INTO title (id, name, name_norm, source, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)", id,
        name, norm, kSourceTag, now, now);
  return id;
}

std::string folder_for(db::Connection& c, const std::vector<std::string>& segments) {
  std::string parent;
  for (size_t i = 0; i < segments.size(); ++i) {
    std::vector<std::string> prefix(segments.begin(), segments.begin() + static_cast<long>(i) + 1);
    auto key = folder_key(prefix);
    auto existing = c.scalar_text("SELECT id FROM folder WHERE path_key = ?", key);
    std::string id;
    if (existing) {
      id = *existing;
    } else {
      id = ids::hash_id(ids::join_key({"folder", key}));
      c.run("INSERT INTO folder (id, parent_id, name, path_key, source) VALUES (?, ?, ?, ?, ?)", id,
            parent.empty() ? std::optional<std::string>() : parent, segments[i], key, kSourceTag);
    }
    parent = id;
  }
  return parent;
}

std::vector<std::string> folder_segments(const std::string& path) {
  std::vector<std::string> out;
  for (auto& s : split_any(path, {"/"}))
    if (!normalize(s).empty()) out.push_back(std::move(s));
  return out;
}

}  // namespace

std::string title_key_id(const std::string& name) {
  return ids::hash_id(ids::join_key({"title", normalize(name)}));
}

std::string folder_key(const std::vector<std::string>& segments) {
  std::vector<std::string> n;
  for (const auto& s : segments) n.push_back(normalize(s));
  return join(n, "/");
}

std::string composition_id(const std::string& work, const std::string& mbz_work_id,
                           const std::vector<Person>& composers) {
  if (!mbz_work_id.empty()) return ids::hash_id(ids::join_key({"composition", "mbid", to_lower_ascii(mbz_work_id)}));
  return ids::hash_id(ids::join_key({"composition", normalize(work), join(sorted_norm_names(composers), ",")}));
}

void upsert_artist(db::Connection& c, const Person& p) {
  c.run(
      "INSERT INTO artist (id, name, sort_name, name_norm, mbz_artist_id) VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
      "sort_name = COALESCE(excluded.sort_name, artist.sort_name), "
      "mbz_artist_id = COALESCE(excluded.mbz_artist_id, artist.mbz_artist_id)",
      artist_id(p.name), p.name, nonempty(p.sort_name), normalize(p.name), nonempty(p.mbid));
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

void write_track_custom(db::Connection& c, const Config& cfg, const TrackInfo& ti, const std::string& track_id,
                        int64_t library_id, int64_t now) {
  (void)cfg;
  // Works (作品): track-level, many-to-many.
  c.run("DELETE FROM track_title WHERE track_id = ? AND source = ?", track_id, kSourceTag);
  for (const auto& name : custom(ti, "collection_work")) {
    auto title_id = title_for(c, name, now);
    if (!title_id.empty())
      c.run("INSERT OR IGNORE INTO track_title (track_id, title_id, source) VALUES (?, ?, ?)", track_id, title_id,
            kSourceTag);
  }

  // Composition -> arrangement: decided by WORK / MUSICBRAINZ_WORKID.
  c.run("DELETE FROM track_arrangement WHERE track_id = ? AND source = ?", track_id, kSourceTag);
  if (!ti.work.empty() || !ti.mbz_work_id.empty()) {
    auto comp_name = ti.work.empty() ? ti.title : ti.work;
    std::string comp_id;
    if (!ti.mbz_work_id.empty())
      comp_id = c.scalar_text("SELECT id FROM composition WHERE mbz_work_id = ?", ti.mbz_work_id).value_or("");
    if (comp_id.empty()) {
      comp_id = composition_id(ti.work, ti.mbz_work_id, ti.composers);
      c.run(
          "INSERT OR IGNORE INTO composition (id, name, name_norm, mbz_work_id, source, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)",
          comp_id, comp_name, normalize(comp_name), nonempty(ti.mbz_work_id), kSourceTag, now, now);
    }
    c.run("DELETE FROM participant WHERE entity_type = 'composition' AND entity_id = ? AND source = ?", comp_id,
          kSourceTag);
    insert_participants(c, "composition", comp_id, "composer", ti.composers);

    const bool implicit = ti.arrangement.empty() && ti.arrangers.empty();
    std::string arr_id;
    if (implicit) {
      arr_id = c.scalar_text("SELECT id FROM arrangement WHERE composition_id = ? AND is_implicit = 1", comp_id)
                   .value_or(ids::hash_id(ids::join_key({"arrangement", "implicit", comp_id})));
    } else {
      arr_id = ids::hash_id(ids::join_key(
          {"arrangement", comp_id, normalize(ti.arrangement), join(sorted_norm_names(ti.arrangers), ",")}));
    }
    c.run(
        "INSERT OR IGNORE INTO arrangement (id, composition_id, name, kind, is_implicit, source, created_at, "
        "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        arr_id, comp_id, nonempty(ti.arrangement), implicit ? "original" : arrangement_kind(ti.arrangement),
        implicit, kSourceTag, now, now);
    if (!implicit) {
      c.run("DELETE FROM participant WHERE entity_type = 'arrangement' AND entity_id = ? AND source = ?", arr_id,
            kSourceTag);
      insert_participants(c, "arrangement", arr_id, "arranger", ti.arrangers);
    }
    c.run("INSERT INTO track_arrangement (track_id, arrangement_id, source) VALUES (?, ?, ?)", track_id, arr_id,
          kSourceTag);
  }

  // Virtual albums: track-level tag; the album is keyed by name and album artist.
  c.run("DELETE FROM album_track WHERE track_id = ? AND source = ?", track_id, kSourceTag);
  for (const auto& name : custom(ti, "collection_album")) {
    if (normalize(name).empty()) continue;
    auto vid = ids::hash_id(ids::join_key({"virtual-album", normalize(name), ti.album_artist_id}));
    c.run(
        "INSERT OR IGNORE INTO album (id, pid, library_id, name, display_artist, release_date, year, kind, "
        "source, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, 'virtual', ?, ?, ?)",
        vid, vid, library_id, name, ti.display_album_artist, nonempty(ti.release_date), ti.year, kSourceTag, now,
        now);
    if (!c.scalar_int("SELECT 1 FROM participant WHERE entity_type = 'album' AND entity_id = ? LIMIT 1", vid))
      insert_participants(c, "album", vid, "albumartist", ti.album_artists);
    c.run(
        "INSERT OR IGNORE INTO album_track (album_id, track_id, disc_number, track_number, source) "
        "VALUES (?, ?, ?, ?, ?)",
        vid, track_id, ti.disc, ti.track, kSourceTag);
  }
}

void update_album_folders(db::Connection& c, const Config& cfg, const std::set<std::string>& album_ids) {
  auto tag = cfg.tags.find("collection");
  if (tag == cfg.tags.end()) return;
  for (const auto& album : album_ids) {
    std::vector<std::string> paths;
    auto st = c.prepare("SELECT tags_json FROM track WHERE album_id = ? AND missing = 0");
    st.bind(1, album);
    while (st.step()) {
      std::map<std::string, std::vector<std::string>> tags;
      try {
        tags = nlohmann::json::parse(st.text(0)).get<std::map<std::string, std::vector<std::string>>>();
      } catch (...) {
        continue;
      }
      for (auto& v : custom_tag_values(tags, tag->second))
        if (std::find(paths.begin(), paths.end(), v) == paths.end()) paths.push_back(std::move(v));
    }
    c.run("DELETE FROM album_folder WHERE album_id = ? AND source = ?", album, kSourceTag);
    for (const auto& path : paths) {
      auto segments = folder_segments(path);
      if (segments.empty()) continue;
      auto folder = folder_for(c, segments);
      c.run("INSERT OR IGNORE INTO album_folder (album_id, folder_id, source) VALUES (?, ?, ?)", album, folder,
            kSourceTag);
    }
  }
}

void finish_custom(db::Connection& c, int64_t now) {
  // Virtual albums: counts over live member tracks that are not excluded by hand.
  c.run(
      "UPDATE album SET "
      "song_count = (SELECT COUNT(DISTINCT t.id) FROM album_track at JOIN track t ON t.id = at.track_id "
      "  WHERE at.album_id = album.id AND at.excluded = 0 AND t.missing = 0 AND NOT EXISTS ("
      "    SELECT 1 FROM album_track x WHERE x.album_id = at.album_id AND x.track_id = at.track_id "
      "    AND x.excluded = 1)), "
      "duration_ms = (SELECT COALESCE(SUM(d), 0) FROM (SELECT DISTINCT t.id, t.duration_ms AS d "
      "  FROM album_track at JOIN track t ON t.id = at.track_id "
      "  WHERE at.album_id = album.id AND at.excluded = 0 AND t.missing = 0)), "
      "updated_at = ? WHERE kind = 'virtual'",
      now);
  c.exec(
      "UPDATE album SET missing = (song_count = 0) WHERE kind = 'virtual';"
      // Tag-made rows that nothing refers to any more.
      "DELETE FROM album WHERE kind = 'virtual' AND source = 2 "
      "  AND id NOT IN (SELECT album_id FROM album_track);"
      "DELETE FROM title WHERE source = 2 AND id NOT IN (SELECT title_id FROM track_title);"
      "DELETE FROM arrangement WHERE source = 2 AND id NOT IN (SELECT arrangement_id FROM track_arrangement);"
      "DELETE FROM composition WHERE source = 2 AND id NOT IN (SELECT composition_id FROM arrangement) "
      "  AND id NOT IN (SELECT from_id FROM relation WHERE from_type = 'composition') "
      "  AND id NOT IN (SELECT to_id FROM relation WHERE to_type = 'composition');"
      "DELETE FROM participant WHERE entity_type = 'composition' AND entity_id NOT IN (SELECT id FROM composition);"
      "DELETE FROM participant WHERE entity_type = 'arrangement' AND entity_id NOT IN (SELECT id FROM arrangement);");
  // Empty tag-made folders, leaves first.
  for (int depth = 0; depth < 32; ++depth) {
    c.exec(
        "DELETE FROM folder WHERE source = 2 AND id NOT IN (SELECT folder_id FROM album_folder) "
        "AND id NOT IN (SELECT parent_id FROM folder WHERE parent_id IS NOT NULL)");
    if (c.changes() == 0) break;
  }
}

}  // namespace eos::scanner
