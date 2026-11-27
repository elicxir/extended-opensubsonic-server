#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "db/database.h"
#include "scanner/track_info.h"

namespace eos::scanner {

inline constexpr int kSourceEstimate = 1;
inline constexpr int kSourceTag = 2;
inline constexpr int kSourceManual = 3;

void upsert_artist(db::Connection& c, const Person& p);
void insert_participants(db::Connection& c, const char* entity_type, const std::string& entity_id,
                         const char* role, const std::vector<Person>& people);

// Tag-derived custom data of one track (design: データモデル / スキャナ). Replaces the
// track's source = 2 rows for works, composition/arrangement and virtual albums.
// Manual (3) and accepted-estimate (1) rows are never touched.
void write_track_custom(db::Connection& c, const Config& cfg, const TrackInfo& ti,
                        const std::string& track_id, int64_t library_id, int64_t now);

// Folders of physical albums from the album-level folder tag, merged over every live
// track of the album.
void update_album_folders(db::Connection& c, const Config& cfg, const std::set<std::string>& album_ids);

// Recomputes virtual album aggregates and removes tag-made rows nothing refers to.
void finish_custom(db::Connection& c, int64_t now);

// Ids used for tag-made entities, stable across scans.
std::string title_key_id(const std::string& name);
std::string folder_key(const std::vector<std::string>& segments);
std::string composition_id(const std::string& work, const std::string& mbz_work_id,
                           const std::vector<Person>& composers);

}  // namespace eos::scanner
