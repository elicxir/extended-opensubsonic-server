#pragma once

#include <optional>
#include <string>
#include <vector>

#include "api/ext/router.h"

namespace eos::api::ext {

// "manual" (3), "tag" (2), or for estimates (1) the method of the suggestion that made
// the row ("rule" / "llm").
std::string provenance(int64_t source, const std::optional<std::string>& method);
// What a link rests on (docs/link-origin.md): tag and manual rows are their source;
// estimated rows say external, audio, name or llm, or nothing when not known.
std::optional<std::string> origin_of(int64_t source, const std::optional<std::string>& origin);
// The origins a suggestion may give (estimates only).
bool valid_origin(const std::string& origin);
// Where a link came from, as the fields of its JSON: provenance, and when known
// confidence, rule, origin and originRef. Columns from `col` on: the suggestion's score
// and rule, then the row's origin and origin_ref (each NULL when none).
json prov_fields(int64_t source, const std::optional<std::string>& method, const db::Statement& st, int col);

// Songs as OpenSubsonic Child objects plus compositionId, arrangementId,
// arrangementProvenance, variationCount, titleIds, virtualAlbumIds, performanceKind, recordedDate.
json songs(ExtCtx& c, const std::vector<std::string>& ids);
// Albums as AlbumID3 plus kind and source.
json albums(ExtCtx& c, const std::vector<std::string>& ids);

json title_json(db::Connection& db, const std::string& id);
json composition_json(db::Connection& db, const std::string& id);
json arrangement_json(db::Connection& db, const std::string& id);
json relation_json(db::Connection& db, const std::string& id, const std::string& seen_from = "");
json folder_json(db::Connection& db, const std::string& id);

// Effective links (manual exclusions applied, rows below `min_source` hidden).
std::vector<std::string> track_title_ids(db::Connection& db, const std::string& track, int min_source);
std::vector<std::string> title_track_ids(db::Connection& db, const std::string& title, int min_source);
std::vector<std::string> folder_album_ids(db::Connection& db, const std::string& folder);
std::vector<std::string> album_folder_ids(db::Connection& db, const std::string& album);
// The arrangement a track belongs to (highest source >= min_source).
std::optional<std::string> track_arrangement(db::Connection& db, const std::string& track, int min_source);

bool exists(db::Connection& db, const char* table, const std::string& id);
void require_track(db::Connection& db, const std::string& id);
int64_t source_of(db::Connection& db, const char* table, const std::string& id);

}  // namespace eos::api::ext
