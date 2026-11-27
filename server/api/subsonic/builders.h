#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "api/node.h"
#include "db/database.h"

namespace eos::api {

// JSON array text for binding id lists: `... IN (SELECT value FROM json_each(?))`.
std::string json_ids(const std::vector<std::string>& ids);

std::string album_cover_id(const std::string& album_id, int64_t updated_ms);
std::string track_cover_id(const std::string& track_id, int64_t updated_ms);

// Loads Child / AlbumID3 / ArtistID3 elements for a list of ids, keeping the order
// (and duplicates) of `ids`. Ids that do not exist are skipped. Missing tracks and
// albums are skipped too.
class Loader {
 public:
  Loader(db::Connection& db, int64_t user_id) : db_(db), user_id_(user_id) {}

  std::vector<Node> songs(const std::vector<std::string>& ids, const std::string& elem = "song");
  std::vector<Node> albums(const std::vector<std::string>& ids, const std::string& elem = "album");
  std::vector<Node> artists(const std::vector<std::string>& ids, const std::string& elem = "artist");

  std::optional<Node> song(const std::string& id, const std::string& elem = "song");
  std::optional<Node> album(const std::string& id, const std::string& elem = "album");
  std::optional<Node> artist(const std::string& id, const std::string& elem = "artist");

  // Track ids of an album in disc / track / path order.
  std::vector<std::string> album_track_ids(const std::string& album_id);
  // Album ids where the artist is an album artist, newest first.
  std::vector<std::string> artist_album_ids(const std::string& artist_id);

  // Collects ids from a query whose first column is the id.
  template <class... Args>
  std::vector<std::string> ids(std::string_view sql, const Args&... args) {
    std::vector<std::string> out;
    auto st = db_.prepare(sql);
    st.bind_all(args...);
    while (st.step()) out.push_back(st.text(0));
    return out;
  }

 private:
  db::Connection& db_;
  int64_t user_id_;
};

}  // namespace eos::api
