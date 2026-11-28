// /ext/artists/{id}/image: a picture of an artist, with where it came from. Pictures are
// put there by hand or by a tool; the server does not look for them. getCoverArt serves
// them under the artist's coverArt id.
#include <algorithm>

#include "api/ext/model.h"
#include "api/subsonic/builders.h"
#include "core/crypto.h"

namespace eos::api::ext {
namespace {

using njson = nlohmann::json;

constexpr size_t kMaxBytes = 10 * 1024 * 1024;

// The image type by its first bytes; empty when it is not one the server serves.
std::string sniff(const crypto::Bytes& b) {
  auto starts = [&](std::string_view magic, size_t at = 0) {
    return b.size() >= at + magic.size() &&
           std::equal(magic.begin(), magic.end(), b.begin() + static_cast<std::ptrdiff_t>(at),
                      [](char m, uint8_t x) { return static_cast<uint8_t>(m) == x; });
  };
  if (starts("\xFF\xD8\xFF")) return "image/jpeg";
  if (starts("\x89PNG\r\n\x1A\n")) return "image/png";
  if (starts("GIF8")) return "image/gif";
  if (starts("RIFF") && starts("WEBP", 8)) return "image/webp";
  return {};
}

void require_artist(db::Connection& db, const std::string& id) {
  if (!exists(db, "artist", id)) throw not_found("Artist not found: " + id);
}

std::optional<std::string> opt_text_field(const njson& b, const char* key) {
  if (!b.contains(key) || b[key].is_null()) return std::nullopt;
  if (!b[key].is_string()) throw bad_request(std::string(key) + " must be a string");
  auto v = b[key].get<std::string>();
  return v.empty() ? std::nullopt : std::optional(std::move(v));
}

json image_json(db::Connection& db, const std::string& artist) {
  auto st = db.prepare(
      "SELECT mime, LENGTH(data), source_url, license, credit, updated_at FROM artist_image WHERE artist_id = ?");
  st.bind(1, artist);
  if (!st.step()) throw not_found("This artist has no picture");
  auto text_or_null = [&](int col) { return st.is_null(col) ? json(nullptr) : json(st.text(col)); };
  return {{"artistId", artist},
          {"coverArt", artist_cover_id(artist, st.int64(5))},
          {"mime", st.text(0)},
          {"size", st.int64(1)},
          {"sourceUrl", text_or_null(2)},
          {"license", text_or_null(3)},
          {"credit", text_or_null(4)},
          {"updated", iso_time(st.int64(5))}};
}

}  // namespace

void add_artist_image_routes(ExtRouter& r) {
  // Every artist, whatever they did (getArtists has the album artists only): id, name,
  // roles, and coverArt for those with a picture.
  r.add("GET", "/ext/artists", [](ExtCtx& c) {
    json items = json::array();
    auto st = c.db.prepare(
        "SELECT a.id, a.name, a.sort_name, a.mbz_artist_id, ai.updated_at, "
        "  (SELECT json_group_array(role) FROM (SELECT DISTINCT p.role AS role FROM participant p "
        "   WHERE p.artist_id = a.id ORDER BY 1)) "
        "FROM artist a LEFT JOIN artist_image ai ON ai.artist_id = a.id ORDER BY COALESCE(a.sort_name, a.name), a.id");
    while (st.step()) {
      json j = {{"id", st.text(0)},
                {"name", st.text(1)},
                {"sortName", st.is_null(2) ? json(nullptr) : json(st.text(2))},
                {"musicBrainzId", st.is_null(3) ? json(nullptr) : json(st.text(3))},
                {"roles", json::parse(st.text(5), nullptr, false)}};
      if (!st.is_null(4)) j["coverArt"] = artist_cover_id(st.text(0), st.int64(4));
      items.push_back(std::move(j));
    }
    return ok({{"items", std::move(items)}});
  });

  // Every artist that has a picture, for tools that fill in the rest.
  r.add("GET", "/ext/artist-images", [](ExtCtx& c) {
    json items = json::array();
    std::vector<std::string> ids;
    {
      auto st = c.db.prepare(
          "SELECT ai.artist_id FROM artist_image ai JOIN artist a ON a.id = ai.artist_id "
          "ORDER BY COALESCE(a.sort_name, a.name)");
      while (st.step()) ids.push_back(st.text(0));
    }
    for (const auto& id : ids) items.push_back(image_json(c.db, id));
    return ok({{"items", std::move(items)}});
  });

  r.add("GET", "/ext/artists/{}/image", [](ExtCtx& c) {
    require_artist(c.db, c.args[0]);
    return ok(image_json(c.db, c.args[0]));
  });

  r.add(
      "PUT", "/ext/artists/{}/image",
      [](ExtCtx& c) {
        const auto& id = c.args[0];
        require_artist(c.db, id);
        const auto& b = c.body();
        if (!b.contains("data") || !b["data"].is_string()) throw bad_request("data (the image, base64) is required");
        auto bytes = crypto::base64_decode(b["data"].get_ref<const std::string&>());
        if (!bytes || bytes->empty()) throw bad_request("data is not base64");
        if (bytes->size() > kMaxBytes) throw bad_request("The image is larger than 10 MB");
        const auto mime = sniff(*bytes);
        if (mime.empty()) throw bad_request("data is not a JPEG, PNG, GIF or WebP image");
        const auto source = opt_text_field(b, "sourceUrl"), license = opt_text_field(b, "license"),
                   credit = opt_text_field(b, "credit");
        c.write([&](db::Connection& w) {
          auto st = w.prepare(
              "INSERT OR REPLACE INTO artist_image (artist_id, mime, data, source_url, license, credit, updated_at) "
              "VALUES (?, ?, ?, ?, ?, ?, ?)");
          st.bind(1, id).bind(2, mime);
          st.bind_blob(3, bytes->data(), bytes->size());
          st.bind(4, source).bind(5, license).bind(6, credit).bind(7, db::now_ms());
          st.run();
        });
        return ok(image_json(c.db, id));
      },
      Access::admin);

  r.add(
      "DELETE", "/ext/artists/{}/image",
      [](ExtCtx& c) {
        require_artist(c.db, c.args[0]);
        c.write([&](db::Connection& w) { w.run("DELETE FROM artist_image WHERE artist_id = ?", c.args[0]); });
        return no_content();
      },
      Access::admin);
}

}  // namespace eos::api::ext
