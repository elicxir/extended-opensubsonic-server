// /ext calls. See docs/ext-api.md.
#include "impl.h"
#include "parse.h"

namespace eos::client {
namespace {

using parse::object;

void add_paging(Query& q, const Paging& p) {
  q.emplace_back("limit", std::to_string(p.limit));
  q.emplace_back("offset", std::to_string(p.offset));
}

template <class F>
auto page(const json& j, F&& f) -> Page<decltype(f(j))> {
  return {parse::list(j, "items", f), parse::get<int64_t>(j, "total")};
}

std::string seg(const std::string& id) { return url_encode(id); }

}  // namespace

LoginResult Api::login(const Options& server, const std::string& username, const std::string& password,
                       const std::string& device_name) {
  Options o = server;
  o.api_key.clear();
  Impl impl(std::move(o));
  const json body = {{"username", username}, {"password", password}, {"deviceName", device_name}};
  auto j = impl.ext("POST", "/ext/auth/login", {}, &body);
  return parsed("/ext/auth/login", [&] {
    return LoginResult{parse::get<std::string>(j, "id"), parse::get<std::string>(j, "key"),
                       parse::get<std::string>(j, "username"), parse::get<bool>(j, "admin")};
  });
}

// ---- reads ----

ServerInfo Api::info() {
  auto j = impl_->ext_get("/ext/info");
  return parsed("/ext/info", [&] {
    return ServerInfo{parse::get<int>(j, "apiVersion"), parse::get<std::string>(j, "serverVersion"),
                      parse::get<std::vector<std::string>>(j, "features")};
  });
}

ExtScanStatus Api::ext_scan_status() {
  auto j = impl_->ext_get("/ext/scan");
  return parsed("/ext/scan", [&] {
    ExtScanStatus s;
    s.scanning = parse::get<bool>(j, "scanning");
    s.phase = parse::get<std::string>(j, "phase");
    s.count = parse::get<int64_t>(j, "count");
    s.folder_count = parse::get<int64_t>(j, "folderCount");
    s.last = parse::maybe(j, "last", [](const json& l) {
      ExtScanStatus::Run r;
      r.kind = parse::get<std::string>(l, "kind");
      r.started_at = parse::get<std::string>(l, "startedAt");
      r.finished_at = parse::opt<std::string>(l, "finishedAt");
      r.added = parse::get<int64_t>(l, "added");
      r.updated = parse::get<int64_t>(l, "updated");
      r.moved = parse::get<int64_t>(l, "moved");
      r.missing = parse::get<int64_t>(l, "missing");
      r.errors = parse::get<int64_t>(l, "errors");
      return r;
    });
    return s;
  });
}

std::vector<Folder> Api::folders() {
  auto j = impl_->ext_get("/ext/folders");
  return parsed("/ext/folders", [&] { return parse::list(j, "items", parse::folder); });
}

FolderDetail Api::folder(const std::string& id) {
  auto j = impl_->ext_get("/ext/folders/" + seg(id));
  return parsed("/ext/folders/{id}", [&] {
    return FolderDetail{parse::folder(object(j, "folder")), parse::list(j, "children", parse::folder),
                        parse::list(j, "albums", parse::album)};
  });
}

Page<Album> Api::ext_albums(const std::string& kind, Paging paging) {
  Query q{{"kind", kind}};
  add_paging(q, paging);
  auto j = impl_->ext_get("/ext/albums", std::move(q));
  return parsed("/ext/albums", [&] { return page(j, parse::album); });
}

AlbumDetail Api::ext_album(const std::string& id) {
  auto j = impl_->ext_get("/ext/albums/" + seg(id));
  return parsed("/ext/albums/{id}", [&] {
    return AlbumDetail{parse::album(j), parse::get<std::vector<std::string>>(j, "folderIds"),
                       parse::list(j, "virtualAlbums", parse::album), parse::list(j, "parentAlbums", parse::album)};
  });
}

std::vector<TitleGroup> Api::album_titles(const std::string& album_id) {
  auto j = impl_->ext_get("/ext/albums/" + seg(album_id) + "/titles");
  return parsed("/ext/albums/{id}/titles", [&] {
    return parse::list(j, "groups", [](const json& g) {
      return TitleGroup{parse::maybe(g, "title", parse::title), parse::list(g, "songs", parse::song)};
    });
  });
}

Page<Title> Api::titles(const std::string& query, Paging paging) {
  Query q;
  if (!query.empty()) q.emplace_back("query", query);
  add_paging(q, paging);
  auto j = impl_->ext_get("/ext/titles", std::move(q));
  return parsed("/ext/titles", [&] { return page(j, parse::title); });
}

TitleDetail Api::title(const std::string& id) {
  auto j = impl_->ext_get("/ext/titles/" + seg(id));
  return parsed("/ext/titles/{id}", [&] {
    return TitleDetail{parse::title(object(j, "title")), parse::list(j, "albums", parse::album),
                       parse::list(j, "songs", parse::song)};
  });
}

Page<Composition> Api::compositions(const std::string& query, Paging paging) {
  Query q;
  if (!query.empty()) q.emplace_back("query", query);
  add_paging(q, paging);
  auto j = impl_->ext_get("/ext/compositions", std::move(q));
  return parsed("/ext/compositions", [&] { return page(j, parse::composition); });
}

CompositionDetail Api::composition(const std::string& id) {
  auto j = impl_->ext_get("/ext/compositions/" + seg(id));
  return parsed("/ext/compositions/{id}", [&] {
    return CompositionDetail{parse::composition(object(j, "composition")),
                             parse::list(j, "arrangements", parse::arrangement_songs),
                             parse::list(j, "relations", parse::relation)};
  });
}

Song Api::ext_song(const std::string& id) {
  auto j = impl_->ext_get("/ext/songs/" + seg(id));
  return parsed("/ext/songs/{id}", [&] { return parse::song(j); });
}

SongVersions Api::song_versions(const std::string& song_id) {
  auto j = impl_->ext_get("/ext/songs/" + seg(song_id) + "/versions");
  return parsed("/ext/songs/{id}/versions", [&] {
    SongVersions v;
    v.song = parse::song(object(j, "song"));
    v.composition = parse::maybe(j, "composition", parse::composition);
    v.arrangements = parse::list(j, "arrangements", parse::arrangement_songs);
    v.performances = parse::list(j, "performances", parse::song);
    v.related = parse::list(j, "related", parse::related_item);
    return v;
  });
}

ExtSearchResult Api::ext_search(const std::string& q, int limit) {
  auto j = impl_->ext_get("/ext/search", {{"q", q}, {"limit", std::to_string(limit)}});
  return parsed("/ext/search", [&] {
    ExtSearchResult r;
    r.compositions = parse::list(j, "compositions", [](const json& c) {
      return ExtSearchResult::CompositionHit{parse::composition(object(c, "composition")),
                                             parse::list(c, "songs", parse::song)};
    });
    r.titles = parse::list(j, "titles", [](const json& t) {
      return ExtSearchResult::TitleHit{parse::title(object(t, "title")), parse::get<int>(t, "songCount"),
                                       parse::list(t, "songs", parse::song)};
    });
    r.songs = parse::list(j, "songs", parse::song);
    r.albums = parse::list(j, "albums", parse::album);
    r.artists = parse::list(j, "artists", parse::artist);
    return r;
  });
}

Graph Api::graph(const GraphQuery& gq) {
  auto csv = [](const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
  };
  Query q{{"seed", gq.seed},
          {"depth", std::to_string(gq.depth)},
          {"limit", std::to_string(gq.limit)},
          {"fanout", std::to_string(gq.fanout)}};
  if (!gq.include.empty()) q.emplace_back("include", csv(gq.include));
  if (!gq.roles.empty()) q.emplace_back("roles", csv(gq.roles));
  auto j = impl_->ext_get("/ext/graph", std::move(q));
  return parsed("/ext/graph", [&] { return parse::graph(j); });
}

// ---- preferred versions ----

std::vector<Preferred> Api::preferred() {
  auto j = impl_->ext("GET", "/ext/preferred");
  return parsed("/ext/preferred", [&] {
    return parse::list(j, "items", [](const json& p) {
      return Preferred{parse::get<std::string>(p, "compositionId"), parse::get<std::string>(p, "songId")};
    });
  });
}

void Api::set_preferred(const std::string& composition_id, const std::string& song_id) {
  const json body = {{"songId", song_id}};
  impl_->ext("PUT", "/ext/compositions/" + seg(composition_id) + "/preferred", {}, &body);
}

void Api::clear_preferred(const std::string& composition_id) {
  impl_->ext("DELETE", "/ext/compositions/" + seg(composition_id) + "/preferred");
}

// ---- API keys ----

std::vector<ApiKeyInfo> Api::api_keys() {
  auto j = impl_->ext("GET", "/ext/apikeys");
  return parsed("/ext/apikeys", [&] {
    return parse::list(j, "items", [](const json& k) {
      return ApiKeyInfo{parse::get<std::string>(k, "id"), parse::get<std::string>(k, "deviceName"),
                        parse::get<std::string>(k, "createdAt"), parse::opt<std::string>(k, "lastUsedAt"),
                        parse::get<bool>(k, "revoked")};
    });
  });
}

NewApiKey Api::create_api_key(const std::string& device_name) {
  const json body = {{"deviceName", device_name}};
  auto j = impl_->ext("POST", "/ext/apikeys", {}, &body);
  return parsed("/ext/apikeys", [&] {
    return NewApiKey{parse::get<std::string>(j, "id"), parse::get<std::string>(j, "key"),
                     parse::get<std::string>(j, "deviceName")};
  });
}

void Api::revoke_api_key(const std::string& id) { impl_->ext("DELETE", "/ext/apikeys/" + seg(id)); }

}  // namespace eos::client
