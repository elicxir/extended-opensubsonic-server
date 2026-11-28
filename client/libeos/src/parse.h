// JSON -> structs. Missing fields keep their defaults; a field of the wrong type
// throws nlohmann::json::exception (Impl turns that into Error(parse)).
#pragma once

#include <nlohmann/json.hpp>

#include "eos/client/types.h"

namespace eos::client::parse {

using nlohmann::json;

template <class T>
T get(const json& j, const char* key, T fallback = {}) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return fallback;
  return it->get<T>();
}

template <class T>
std::optional<T> opt(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return std::nullopt;
  return it->get<T>();
}

// j[key] as a list of F(element); a missing or null key gives an empty list.
template <class F>
auto list(const json& j, const char* key, F&& f) -> std::vector<decltype(f(j))> {
  std::vector<decltype(f(j))> out;
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return out;
  out.reserve(it->size());
  for (const auto& e : *it) out.push_back(f(e));
  return out;
}

template <class F>
auto maybe(const json& j, const char* key, F&& f) -> std::optional<decltype(f(j))> {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return std::nullopt;
  return f(*it);
}

// j[key], or an empty object when it is missing.
const json& object(const json& j, const char* key);

ArtistRef artist_ref(const json& j);
Song song(const json& j);
Album album(const json& j);    // reads "song" into songs when present
Artist artist(const json& j);  // reads "album" into albums when present
Genre genre(const json& j);
Playlist playlist(const json& j);  // reads "entry" into songs when present
PlayQueue play_queue(const json& j);
ScanStatus scan_status(const json& j);
SearchResult search_result(const json& j);

Folder folder(const json& j);
Title title(const json& j);
Composition composition(const json& j);
Arrangement arrangement(const json& j);
ArrangementSongs arrangement_songs(const json& j);
Relation relation(const json& j);
RelatedItem related_item(const json& j);
Graph graph(const json& j);
LinkInfo link_info(const json& j);
SongDetail song_detail(const json& j);

}  // namespace eos::client::parse
