// Manual edits through /ext (design: 独自拡張API / 書き込み). Everything written here is
// source = 3. Rows made from tags cannot be deleted (409 tag_managed): the next scan
// would bring them back; unlinking uses `excluded` rows instead.
#include <functional>

#include "api/ext/model.h"
#include "api/ext/suggestions.h"
#include "core/ids.h"
#include "core/normalize.h"
#include "scanner/derived.h"
#include "scanner/track_info.h"

namespace eos::api::ext {
namespace {

using njson = nlohmann::json;
constexpr int kManual = 3;

std::optional<std::string> opt_str(const njson& b, const char* key) {
  if (!b.contains(key) || b[key].is_null()) return std::nullopt;
  if (!b[key].is_string()) throw bad_request(std::string(key) + " must be a string");
  return b[key].get<std::string>();
}

std::string req_str(const njson& b, const char* key) {
  auto v = opt_str(b, key);
  if (!v || normalize(*v).empty()) throw bad_request(std::string(key) + " is required");
  return *v;
}

std::vector<std::string> str_list(const njson& b, const char* key) {
  std::vector<std::string> out;
  if (!b.contains(key)) return out;
  if (!b[key].is_array()) throw bad_request(std::string(key) + " must be an array");
  for (const auto& v : b[key]) {
    if (v.is_string()) out.push_back(v.get<std::string>());
    else if (v.is_object() && v.contains("id")) out.push_back(v["id"].get<std::string>());
    else throw bad_request(std::string(key) + " must hold ids");
  }
  return out;
}

// Replaces the manual people of an entity (composers / arrangers) by name.
void set_people(db::Connection& w, const char* type, const std::string& id, const char* role,
                const std::vector<std::string>& names) {
  w.run("DELETE FROM participant WHERE entity_type = ? AND entity_id = ? AND role = ? AND source = ?", type, id, role,
        kManual);
  int pos = 0;
  for (const auto& n : names) {
    scanner::Person p{n, "", "", ""};
    scanner::upsert_artist(w, p);
    w.run(
        "INSERT OR IGNORE INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source) "
        "VALUES (?, ?, ?, ?, '', ?, ?)",
        type, id, scanner::artist_id(n), role, pos++, kManual);
  }
}

// add / remove for many-to-many links that tags also write (track_title, album_folder,
// album_track). Removing a tag link stores a manual "excluded" row.
void patch_links(db::Connection& w, const char* table, const char* owner_col, const std::string& owner,
                 const char* target_col, const std::vector<std::string>& add, const std::vector<std::string>& remove) {
  for (const auto& t : add) {
    w.run(std::format("INSERT OR REPLACE INTO {} ({}, {}, source, excluded) VALUES (?, ?, ?, 0)", table, owner_col,
                      target_col),
          owner, t, kManual);
  }
  for (const auto& t : remove) {
    bool other = w.scalar_int(std::format("SELECT 1 FROM {} WHERE {} = ? AND {} = ? AND source <> ?", table, owner_col,
                                          target_col),
                              owner, t, kManual)
                     .has_value();
    if (other)
      w.run(std::format("INSERT OR REPLACE INTO {} ({}, {}, source, excluded) VALUES (?, ?, ?, 1)", table, owner_col,
                        target_col),
            owner, t, kManual);
    else
      w.run(std::format("DELETE FROM {} WHERE {} = ? AND {} = ? AND source = ?", table, owner_col, target_col), owner,
            t, kManual);
  }
}

// field_override + the live column. A null value drops the override and restores the
// tag value via `restore`.
void set_override(db::Connection& w, const char* entity, const std::string& id, const char* field,
                  const char* table, const char* column, const njson& value,
                  const std::function<std::optional<std::string>()>& restore) {
  if (value.is_null()) {
    w.run("DELETE FROM field_override WHERE entity_type = ? AND entity_id = ? AND field = ?", entity, id, field);
    w.run(std::format("UPDATE {} SET {} = ? WHERE id = ?", table, column), restore(), id);
    return;
  }
  if (!value.is_string()) throw bad_request(std::string(field) + " must be a string or null");
  auto v = value.get<std::string>();
  w.run("INSERT OR REPLACE INTO field_override (entity_type, entity_id, field, value, updated_at) VALUES (?, ?, ?, ?, ?)",
        entity, id, field, v, db::now_ms());
  w.run(std::format("UPDATE {} SET {} = ? WHERE id = ?", table, column), v, id);
}

std::optional<std::string> tag_value(db::Connection& w, const std::string& track_id, const char* tag) {
  auto json_text = w.scalar_text("SELECT tags_json FROM track WHERE id = ?", track_id);
  if (!json_text) return std::nullopt;
  auto j = njson::parse(*json_text, nullptr, false);
  if (j.contains(tag) && j[tag].is_array() && !j[tag].empty()) return j[tag][0].get<std::string>();
  return std::nullopt;
}

void recompute_path_keys(db::Connection& w, const std::string& folder) {
  auto st = w.prepare("SELECT name, parent_id FROM folder WHERE id = ?");
  st.bind(1, folder);
  if (!st.step()) return;
  auto name = st.text(0);
  auto parent = st.opt_text(1);
  std::string key = normalize(name);
  if (parent) key = *w.scalar_text("SELECT path_key FROM folder WHERE id = ?", *parent) + "/" + key;
  w.run("UPDATE folder SET path_key = ? WHERE id = ?", key, folder);
  std::vector<std::string> children;
  auto ch = w.prepare("SELECT id FROM folder WHERE parent_id = ?");
  ch.bind(1, folder);
  while (ch.step()) children.push_back(ch.text(0));
  for (const auto& c : children) recompute_path_keys(w, c);
}

}  // namespace

void add_write_routes(ExtRouter& r) {
  // -- relations
  r.add(
      "POST", "/ext/relations",
      [](ExtCtx& c) {
        auto b = c.body();
        njson p = {{"action", "relation"}, {"type", b.value("type", "")}, {"from", b.value("from", njson())},
                   {"to", b.value("to", njson())}};
        validate_proposal(c.db, p);
        if (p["from"] == p["to"]) throw bad_request("A relation needs two different items");
        auto id = ids::random_id();
        auto now = db::now_ms();
        c.write([&](db::Connection& w) {
          w.run(
              "INSERT INTO relation (id, from_type, from_id, to_type, to_id, type, subtype, position, note, source, "
              "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
              id, p["from"]["type"].get<std::string>(), p["from"]["id"].get<std::string>(),
              p["to"]["type"].get<std::string>(), p["to"]["id"].get<std::string>(), p["type"].get<std::string>(),
              opt_str(b, "subtype"),
              b.contains("position") && b["position"].is_number_integer() ? std::optional(b["position"].get<int64_t>())
                                                                          : std::nullopt,
              opt_str(b, "note"), kManual, now, now);
        });
        return created(relation_json(c.db, id));
      },
      Access::admin);

  r.add(
      "PUT", "/ext/relations/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        relation_json(c.db, id);  // 404
        auto b = c.body();
        c.write([&](db::Connection& w) {
          if (b.contains("type")) {
            auto type = b["type"].get<std::string>();
            validate_proposal(w, {{"action", "relation"}, {"type", type}, {"from", relation_json(w, id)["from"]},
                                  {"to", relation_json(w, id)["to"]}});
            w.run("UPDATE relation SET type = ? WHERE id = ?", type, id);
          }
          if (b.contains("subtype")) w.run("UPDATE relation SET subtype = ? WHERE id = ?", opt_str(b, "subtype"), id);
          if (b.contains("note")) w.run("UPDATE relation SET note = ? WHERE id = ?", opt_str(b, "note"), id);
          if (b.contains("position"))
            w.run("UPDATE relation SET position = ? WHERE id = ?",
                  b["position"].is_number_integer() ? std::optional(b["position"].get<int64_t>()) : std::nullopt, id);
          w.run("UPDATE relation SET source = ?, updated_at = ? WHERE id = ?", kManual, db::now_ms(), id);
        });
        return ok(relation_json(c.db, id));
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/relations/{}",
      [](ExtCtx& c) {
        relation_json(c.db, c.args[0]);
        c.write([&](db::Connection& w) { w.run("DELETE FROM relation WHERE id = ?", c.args[0]); });
        return no_content();
      },
      Access::admin);

  // -- compositions and arrangements
  r.add(
      "POST", "/ext/compositions",
      [](ExtCtx& c) {
        auto b = c.body();
        auto name = req_str(b, "name");
        auto mbid = opt_str(b, "musicBrainzWorkId");
        auto id = ids::random_id();
        c.write(
            [&](db::Connection& w) {
              auto now = db::now_ms();
              w.run(
                  "INSERT INTO composition (id, name, name_norm, mbz_work_id, source, created_at, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?)",
                  id, name, normalize(name), mbid, kManual, now, now);
              w.run(
                  "INSERT INTO arrangement (id, composition_id, kind, is_implicit, source, created_at, updated_at) "
                  "VALUES (?, ?, 'original', 1, ?, ?, ?)",
                  ids::random_id(), id, kManual, now, now);
              set_people(w, "composition", id, "composer", str_list(b, "composers"));
            },
            true);
        return created(composition_json(c.db, id));
      },
      Access::admin);

  r.add(
      "PUT", "/ext/compositions/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        composition_json(c.db, id);
        auto b = c.body();
        c.write(
            [&](db::Connection& w) {
              if (auto name = opt_str(b, "name"))
                w.run("UPDATE composition SET name = ?, name_norm = ? WHERE id = ?", *name, normalize(*name), id);
              if (b.contains("musicBrainzWorkId"))
                w.run("UPDATE composition SET mbz_work_id = ? WHERE id = ?", opt_str(b, "musicBrainzWorkId"), id);
              if (b.contains("composers")) set_people(w, "composition", id, "composer", str_list(b, "composers"));
              // Edited by hand: keep it even if the tags stop referring to it.
              w.run("UPDATE composition SET source = ?, updated_at = ? WHERE id = ?", kManual, db::now_ms(), id);
            },
            true);
        return ok(composition_json(c.db, id));
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/compositions/{}",
      [](ExtCtx& c) {
        if (source_of(c.db, "composition", c.args[0]) == 2) throw tag_managed("This composition comes from tags");
        c.write([&](db::Connection& w) { w.run("DELETE FROM composition WHERE id = ?", c.args[0]); }, true);
        return no_content();
      },
      Access::admin);

  r.add(
      "POST", "/ext/compositions/{}/arrangements",
      [](ExtCtx& c) {
        const auto& comp = c.args[0];
        composition_json(c.db, comp);
        auto b = c.body();
        auto name = req_str(b, "name");
        auto kind = opt_str(b, "kind").value_or(scanner::arrangement_kind(name));
        auto id = ids::random_id();
        c.write([&](db::Connection& w) {
          auto now = db::now_ms();
          w.run(
              "INSERT INTO arrangement (id, composition_id, name, kind, is_implicit, source, created_at, updated_at) "
              "VALUES (?, ?, ?, ?, 0, ?, ?, ?)",
              id, comp, name, kind, kManual, now, now);
          set_people(w, "arrangement", id, "arranger", str_list(b, "arrangers"));
        });
        return created(arrangement_json(c.db, id));
      },
      Access::admin);

  r.add(
      "PUT", "/ext/arrangements/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        arrangement_json(c.db, id);
        auto b = c.body();
        c.write([&](db::Connection& w) {
          if (b.contains("name")) w.run("UPDATE arrangement SET name = ? WHERE id = ?", opt_str(b, "name"), id);
          if (auto kind = opt_str(b, "kind")) w.run("UPDATE arrangement SET kind = ? WHERE id = ?", *kind, id);
          if (b.contains("arrangers")) set_people(w, "arrangement", id, "arranger", str_list(b, "arrangers"));
          w.run("UPDATE arrangement SET source = ?, updated_at = ? WHERE id = ?", kManual, db::now_ms(), id);
        });
        return ok(arrangement_json(c.db, id));
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/arrangements/{}",
      [](ExtCtx& c) {
        if (source_of(c.db, "arrangement", c.args[0]) == 2) throw tag_managed("This arrangement comes from tags");
        c.write([&](db::Connection& w) { w.run("DELETE FROM arrangement WHERE id = ?", c.args[0]); });
        return no_content();
      },
      Access::admin);

  r.add(
      "PUT", "/ext/songs/{}/arrangement",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        require_track(c.db, id);
        auto arrangement = req_str(c.body(), "arrangementId");
        if (!exists(c.db, "arrangement", arrangement)) throw not_found("Arrangement not found");
        c.write([&](db::Connection& w) {
          w.run("INSERT OR REPLACE INTO track_arrangement (track_id, arrangement_id, source) VALUES (?, ?, ?)", id,
                arrangement, kManual);
        });
        return ok(songs(c, {id})[0]);
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/songs/{}/arrangement",
      [](ExtCtx& c) {
        require_track(c.db, c.args[0]);
        c.write([&](db::Connection& w) {
          w.run("DELETE FROM track_arrangement WHERE track_id = ? AND source = ?", c.args[0], kManual);
        });
        return ok(songs(c, {c.args[0]})[0]);
      },
      Access::admin);

  // -- titles (works)
  r.add(
      "POST", "/ext/titles",
      [](ExtCtx& c) {
        auto b = c.body();
        auto name = req_str(b, "name");
        if (c.db.scalar_int("SELECT 1 FROM title WHERE name_norm = ?", normalize(name)))
          throw conflict("A work with this name exists");
        auto id = ids::random_id();
        c.write(
            [&](db::Connection& w) {
              auto now = db::now_ms();
              w.run(
                  "INSERT INTO title (id, name, sort_name, name_norm, kind, year, source, created_at, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                  id, name, opt_str(b, "sortName"), normalize(name), opt_str(b, "kind"),
                  b.contains("year") && b["year"].is_number_integer() ? std::optional(b["year"].get<int64_t>())
                                                                      : std::nullopt,
                  kManual, now, now);
            },
            true);
        return created(title_json(c.db, id));
      },
      Access::admin);

  r.add(
      "PUT", "/ext/titles/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        title_json(c.db, id);
        auto b = c.body();
        c.write(
            [&](db::Connection& w) {
              if (auto name = opt_str(b, "name")) {
                auto other = w.scalar_text("SELECT id FROM title WHERE name_norm = ?", normalize(*name));
                if (other && *other != id) throw conflict("A work with this name exists");
                w.run("UPDATE title SET name = ?, name_norm = ? WHERE id = ?", *name, normalize(*name), id);
              }
              if (b.contains("sortName")) w.run("UPDATE title SET sort_name = ? WHERE id = ?", opt_str(b, "sortName"), id);
              if (b.contains("kind")) w.run("UPDATE title SET kind = ? WHERE id = ?", opt_str(b, "kind"), id);
              if (b.contains("year"))
                w.run("UPDATE title SET year = ? WHERE id = ?",
                      b["year"].is_number_integer() ? std::optional(b["year"].get<int64_t>()) : std::nullopt, id);
              w.run("UPDATE title SET source = ?, updated_at = ? WHERE id = ?", kManual, db::now_ms(), id);
            },
            true);
        return ok(title_json(c.db, id));
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/titles/{}",
      [](ExtCtx& c) {
        if (source_of(c.db, "title", c.args[0]) == 2) throw tag_managed("This work comes from tags");
        c.write([&](db::Connection& w) { w.run("DELETE FROM title WHERE id = ?", c.args[0]); }, true);
        return no_content();
      },
      Access::admin);

  r.add(
      "PATCH", "/ext/songs/{}/titles",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        require_track(c.db, id);
        auto b = c.body();
        auto add = str_list(b, "add"), remove = str_list(b, "remove");
        for (const auto& t : add)
          if (!exists(c.db, "title", t)) throw not_found("Title not found: " + t);
        c.write([&](db::Connection& w) { patch_links(w, "track_title", "track_id", id, "title_id", add, remove); });
        return ok(songs(c, {id})[0]);
      },
      Access::admin);

  // -- folders
  r.add(
      "POST", "/ext/folders",
      [](ExtCtx& c) {
        auto b = c.body();
        auto name = req_str(b, "name");
        auto parent = opt_str(b, "parentId");
        std::string key = normalize(name);
        if (parent) {
          auto pk = c.db.scalar_text("SELECT path_key FROM folder WHERE id = ?", *parent);
          if (!pk) throw not_found("Parent folder not found");
          key = *pk + "/" + key;
        }
        if (c.db.scalar_int("SELECT 1 FROM folder WHERE path_key = ?", key)) throw conflict("This folder exists");
        auto id = ids::random_id();
        c.write([&](db::Connection& w) {
          w.run("INSERT INTO folder (id, parent_id, name, path_key, sort_order, source) VALUES (?, ?, ?, ?, ?, ?)", id,
                parent, name, key,
                b.contains("sortOrder") && b["sortOrder"].is_number_integer()
                    ? std::optional(b["sortOrder"].get<int64_t>())
                    : std::nullopt,
                kManual);
        });
        return created(folder_json(c.db, id));
      },
      Access::admin);

  r.add(
      "PUT", "/ext/folders/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        auto source = source_of(c.db, "folder", id);
        auto b = c.body();
        if (source == 2 && (b.contains("name") || b.contains("parentId")))
          throw tag_managed("This folder comes from tags; only sortOrder can change");
        c.write([&](db::Connection& w) {
          if (b.contains("sortOrder"))
            w.run("UPDATE folder SET sort_order = ? WHERE id = ?",
                  b["sortOrder"].is_number_integer() ? std::optional(b["sortOrder"].get<int64_t>()) : std::nullopt, id);
          if (auto name = opt_str(b, "name")) w.run("UPDATE folder SET name = ? WHERE id = ?", *name, id);
          if (b.contains("parentId")) {
            auto parent = opt_str(b, "parentId");
            for (auto p = parent; p; p = w.scalar_text("SELECT parent_id FROM folder WHERE id = ?", *p))
              if (*p == id) throw bad_request("A folder cannot be moved into itself");
            w.run("UPDATE folder SET parent_id = ? WHERE id = ?", parent, id);
          }
          if (b.contains("name") || b.contains("parentId")) recompute_path_keys(w, id);
        });
        return ok(folder_json(c.db, id));
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/folders/{}",
      [](ExtCtx& c) {
        if (source_of(c.db, "folder", c.args[0]) == 2) throw tag_managed("This folder comes from tags");
        c.write([&](db::Connection& w) { w.run("DELETE FROM folder WHERE id = ?", c.args[0]); });
        return no_content();
      },
      Access::admin);

  r.add(
      "PATCH", "/ext/albums/{}/folders",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        if (!exists(c.db, "album", id)) throw not_found("Album not found");
        auto b = c.body();
        auto add = str_list(b, "add"), remove = str_list(b, "remove");
        for (const auto& f : add)
          if (!exists(c.db, "folder", f)) throw not_found("Folder not found: " + f);
        c.write([&](db::Connection& w) { patch_links(w, "album_folder", "album_id", id, "folder_id", add, remove); });
        return ok({{"albumId", id}, {"folderIds", album_folder_ids(c.db, id)}});
      },
      Access::admin);

  // -- albums
  r.add(
      "POST", "/ext/albums",
      [](ExtCtx& c) {
        auto b = c.body();
        auto name = req_str(b, "name");
        auto tracks = str_list(b, "tracks");
        for (const auto& t : tracks) require_track(c.db, t);
        auto lib = c.db.scalar_int("SELECT MIN(id) FROM library").value_or(1);
        auto id = ids::random_id();
        c.write(
            [&](db::Connection& w) {
              auto now = db::now_ms();
              w.run(
                  "INSERT INTO album (id, pid, library_id, name, sort_name, display_artist, kind, source, created_at, "
                  "updated_at) VALUES (?, ?, ?, ?, ?, ?, 'virtual', ?, ?, ?)",
                  id, id, lib, name, opt_str(b, "sortName"), opt_str(b, "albumArtist"), kManual, now, now);
              if (auto artist = opt_str(b, "albumArtist")) {
                scanner::Person p{*artist, "", "", ""};
                scanner::upsert_artist(w, p);
                w.run(
                    "INSERT INTO participant (entity_type, entity_id, artist_id, role, sub_role, position, source) "
                    "VALUES ('album', ?, ?, 'albumartist', '', 0, ?)",
                    id, scanner::artist_id(*artist), kManual);
              }
              int64_t n = 0;
              for (const auto& t : tracks)
                w.run(
                    "INSERT OR REPLACE INTO album_track (album_id, track_id, track_number, source) VALUES (?, ?, ?, ?)",
                    id, t, ++n, kManual);
              scanner::finish_custom(w, now);
            },
            true);
        return created(albums(c, {id})[0]);
      },
      Access::admin);

  auto patch_album = [](ExtCtx& c) {
    const auto& id = c.args[0];
    if (!exists(c.db, "album", id)) throw not_found("Album not found");
    auto b = c.body();
    c.write(
        [&](db::Connection& w) {
          auto first = w.scalar_text("SELECT id FROM track WHERE album_id = ? ORDER BY path LIMIT 1", id);
          auto restore = [&](const char* tag) {
            return [&w, first, tag]() -> std::optional<std::string> {
              return first ? tag_value(w, *first, tag) : std::nullopt;
            };
          };
          if (b.contains("name")) {
            auto current = w.scalar_text("SELECT name FROM album WHERE id = ?", id);
            set_override(w, "album", id, "name", "album", "name", b["name"], [&]() -> std::optional<std::string> {
              if (!first) return current;  // virtual albums have no tag value to go back to
              return tag_value(w, *first, "ALBUM").value_or(std::string(scanner::kUnknownAlbum));
            });
          }
          if (b.contains("sortName")) set_override(w, "album", id, "sort_name", "album", "sort_name", b["sortName"], restore("ALBUMSORT"));
          w.run("UPDATE album SET updated_at = ? WHERE id = ?", db::now_ms(), id);
        },
        true);
    return ok(albums(c, {id})[0]);
  };
  r.add("PATCH", "/ext/albums/{}", patch_album, Access::admin);
  r.add("PUT", "/ext/albums/{}", patch_album, Access::admin);

  r.add(
      "DELETE", "/ext/albums/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        auto st = c.db.prepare("SELECT kind, source FROM album WHERE id = ?");
        st.bind(1, id);
        if (!st.step()) throw not_found("Album not found");
        if (st.text(0) == "physical") throw tag_managed("Physical albums come from the music files");
        if (st.int64(1) == 2) throw tag_managed("This virtual album comes from tags");
        c.write([&](db::Connection& w) { w.run("DELETE FROM album WHERE id = ?", id); }, true);
        return no_content();
      },
      Access::admin);

  r.add(
      "PATCH", "/ext/albums/{}/tracks",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        auto kind = c.db.scalar_text("SELECT kind FROM album WHERE id = ?", id);
        if (!kind) throw not_found("Album not found");
        if (*kind != "virtual") throw conflict("Only virtual albums have editable track lists");
        auto b = c.body();
        auto remove = str_list(b, "remove");
        struct Add {
          std::string id;
          std::optional<int64_t> disc, track;
        };
        std::vector<Add> add;
        if (b.contains("add")) {
          for (const auto& v : b["add"]) {
            if (v.is_string()) add.push_back({v.get<std::string>(), std::nullopt, std::nullopt});
            else if (v.is_object())
              add.push_back({v.value("id", ""),
                             v.contains("disc") ? std::optional(v["disc"].get<int64_t>()) : std::nullopt,
                             v.contains("track") ? std::optional(v["track"].get<int64_t>()) : std::nullopt});
          }
        }
        for (const auto& a : add) require_track(c.db, a.id);
        c.write([&](db::Connection& w) {
          std::vector<std::string> ids;
          for (const auto& a : add) ids.push_back(a.id);
          patch_links(w, "album_track", "album_id", id, "track_id", ids, remove);
          for (const auto& a : add)
            w.run("UPDATE album_track SET disc_number = ?, track_number = ? WHERE album_id = ? AND track_id = ? AND source = ?",
                  a.disc, a.track, id, a.id, kManual);
          scanner::finish_custom(w, db::now_ms());
        });
        auto list = albums(c, {id});
        return ok(list.empty() ? json{{"id", id}, {"songCount", 0}} : list[0]);
      },
      Access::admin);

  // -- song overrides
  r.add(
      "PATCH", "/ext/songs/{}",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        require_track(c.db, id);
        auto b = c.body();
        c.write(
            [&](db::Connection& w) {
              auto restore = [&](const char* tag) {
                return [&w, id, tag]() { return tag_value(w, id, tag); };
              };
              if (b.contains("title"))
                set_override(w, "track", id, "title", "track", "title", b["title"], [&]() -> std::optional<std::string> {
                  if (auto v = tag_value(w, id, "TITLE")) return v;
                  auto path = w.scalar_text("SELECT path FROM track WHERE id = ?", id).value_or("");
                  return std::filesystem::path(path).stem().string();
                });
              if (b.contains("sortTitle")) set_override(w, "track", id, "sort_title", "track", "sort_title", b["sortTitle"], restore("TITLESORT"));
              if (b.contains("subtitle")) set_override(w, "track", id, "subtitle", "track", "subtitle", b["subtitle"], restore("SUBTITLE"));
              w.run("UPDATE track SET updated_at = ? WHERE id = ?", db::now_ms(), id);
            },
            true);
        return ok(songs(c, {id})[0]);
      },
      Access::admin);

  // -- preferred version (any user, for themselves)
  r.add("PUT", "/ext/compositions/{}/preferred", [](ExtCtx& c) {
    const auto& comp = c.args[0];
    composition_json(c.db, comp);
    auto song = req_str(c.body(), "songId");
    require_track(c.db, song);
    c.write([&](db::Connection& w) {
      w.run("INSERT OR REPLACE INTO preferred_version (user_id, composition_id, track_id) VALUES (?, ?, ?)", c.user.id,
            comp, song);
    });
    return ok({{"compositionId", comp}, {"songId", song}});
  });
  r.add("DELETE", "/ext/compositions/{}/preferred", [](ExtCtx& c) {
    c.write([&](db::Connection& w) {
      w.run("DELETE FROM preferred_version WHERE user_id = ? AND composition_id = ?", c.user.id, c.args[0]);
    });
    return no_content();
  });
  r.add("GET", "/ext/preferred", [](ExtCtx& c) {
    json items = json::array();
    auto st = c.db.prepare("SELECT composition_id, track_id FROM preferred_version WHERE user_id = ?");
    st.bind(1, c.user.id);
    while (st.step()) items.push_back({{"compositionId", st.text(0)}, {"songId", st.text(1)}});
    auto n = static_cast<int64_t>(items.size());
    return page(std::move(items), n);
  });
}

}  // namespace eos::api::ext
