// stream, download, getCoverArt.
#include "api/subsonic/router.h"
#include "core/strings.h"
#include "media/artwork.h"

namespace eos::api {
namespace {

std::filesystem::path require_track_file(Ctx& c, std::string& content_type) {
  auto id = c.require("id");
  auto file = media::track_file(c.db, id);
  if (!file) throw SubsonicError(err::not_found, "Song not found");
  content_type = c.db.scalar_text("SELECT content_type FROM track WHERE id = ?", id).value_or("application/octet-stream");
  return *file;
}

std::string content_disposition(const std::string& filename) {
  std::string ascii, encoded;
  static constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char ch : filename) {
    ascii.push_back(ch < 0x80 && ch >= 0x20 && ch != '"' && ch != '\\' ? static_cast<char>(ch) : '_');
    if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_') {
      encoded.push_back(static_cast<char>(ch));
    } else {
      encoded += '%';
      encoded += hex[ch >> 4];
      encoded += hex[ch & 15];
    }
  }
  return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

}  // namespace

void add_media_handlers(HandlerTable& t) {
  t.add_raw("stream", [](Ctx& c) {
    std::string type;
    auto file = require_track_file(c, type);
    auto id = c.require("id");
    auto st = c.db.prepare("SELECT suffix, COALESCE(bit_rate, 0) FROM track WHERE id = ?");
    st.bind(1, id);
    st.step();
    auto plan = media::plan_transcode(c.svc.cfg, st.text(0), static_cast<int>(st.int64(1)), c.param("format"),
                                      c.opt_int("maxBitRate"), c.opt_int("timeOffset").value_or(0));
    if (!plan) return file_response(c.req, file, type);  // timeOffset needs no work: clients use Range
    auto source = c.svc.transcoder.start(*plan, file);
    if (!source) throw SubsonicError(err::generic, "Too many transcodes running; try again shortly");
    Response r;
    r.content_type = plan->content_type;
    r.stream = std::move(source);
    return r;
  });

  t.add_raw("download", [](Ctx& c) {
    std::string type;
    auto file = require_track_file(c, type);
    auto r = file_response(c.req, file, type);
    r.headers.emplace_back("Content-Disposition", content_disposition(file.filename().string()));
    return r;
  });

  t.add_raw("getCoverArt", [](Ctx& c) {
    auto raw_id = c.require("id");
    auto size = static_cast<int>(c.int_param("size", 0, 0, 1200));
    std::string kind, id = raw_id;
    bool versioned = false;
    if (raw_id.size() > 3 && raw_id[2] == '-') {
      kind = raw_id.substr(0, 2);
      id = raw_id.substr(3);
      if (auto us = id.rfind('_'); us != std::string::npos) {
        versioned = true;
        id.resize(us);
      }
    }
    std::optional<media::Image> img;
    auto& art = c.svc.artwork;
    if (kind == "al") {
      img = art.album(c.db, id, size);
    } else if (kind == "mf") {
      img = art.track(c.db, id, size);
    } else if (kind == "ar") {
      img = art.artist(c.db, id, size);
    } else if (kind.empty()) {
      if (c.db.scalar_int("SELECT 1 FROM album WHERE id = ?", id)) img = art.album(c.db, id, size);
      else if (c.db.scalar_int("SELECT 1 FROM track WHERE id = ?", id)) img = art.track(c.db, id, size);
      else img = art.artist(c.db, id, size);
    }
    if (!img) throw SubsonicError(err::not_found, "Artwork not found");
    Response r;
    r.content_type = img->mime;
    r.body = std::move(img->data);
    r.headers.emplace_back("Cache-Control",
                           versioned ? "public, max-age=31536000, immutable" : "public, max-age=3600");
    return r;
  });
}

}  // namespace eos::api
