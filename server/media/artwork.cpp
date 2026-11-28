#include "media/artwork.h"

#include <taglib/fileref.h>
#include <taglib/tvariant.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include <stb/stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb/stb_image_write.h>

#include "core/crypto.h"
#include "core/log.h"
#include "core/strings.h"

namespace eos::media {
namespace fs = std::filesystem;

namespace {

std::string mime_for(const fs::path& p) {
  auto ext = to_lower_ascii(p.extension().string());
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".bmp") return "image/bmp";
  return {};
}

std::optional<std::string> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int64_t mtime_of(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  return ec ? 0 : t.time_since_epoch().count();
}

}  // namespace

std::optional<fs::path> track_file(db::Connection& db, const std::string& track_id) {
  auto st = db.prepare(
      "SELECT l.path, t.path FROM track t JOIN library l ON l.id = t.library_id "
      "WHERE t.id = ? AND t.missing = 0");
  st.bind(1, track_id);
  if (!st.step()) return std::nullopt;
  std::error_code ec;
  auto root = fs::weakly_canonical(st.text(0), ec);
  auto file = fs::weakly_canonical(fs::path(st.text(0)) / st.text(1), ec);
  if (ec) return std::nullopt;
  auto rel = file.lexically_relative(root);
  if (rel.empty() || rel.native().starts_with("..")) return std::nullopt;  // escapes the library
  return file;
}

std::optional<fs::path> library_image(db::Connection& db, const std::string& album_id, const std::string& relative) {
  auto lib = db.scalar_text("SELECT l.path FROM album al JOIN library l ON l.id = al.library_id WHERE al.id = ?", album_id);
  if (!lib || relative.empty()) return std::nullopt;
  std::error_code ec;
  auto root = fs::weakly_canonical(*lib, ec);
  auto file = fs::weakly_canonical(fs::path(*lib) / fs::path(std::u8string(relative.begin(), relative.end())), ec);
  if (ec) return std::nullopt;
  auto rel = file.lexically_relative(root);
  if (rel.empty() || rel.native().starts_with("..")) return std::nullopt;  // escapes the library
  if (!fs::is_regular_file(file, ec) || mime_for(file).empty()) return std::nullopt;
  return file;
}

Artwork::Artwork(const Config& cfg) : cfg_(cfg), cache_dir_(cfg.cache_dir() / "artwork") {}

std::optional<Image> Artwork::embedded(const fs::path& file) {
  TagLib::FileRef ref(file.c_str(), false);
  if (ref.isNull()) return std::nullopt;
  auto pictures = ref.complexProperties("PICTURE");
  const TagLib::VariantMap* best = nullptr;
  for (const auto& pic : pictures) {
    if (!best) best = &pic;
    auto type = pic.value("pictureType").value<TagLib::String>();
    if (type == "Front Cover") {
      best = &pic;
      break;
    }
  }
  if (!best) return std::nullopt;
  auto data = best->value("data").value<TagLib::ByteVector>();
  if (data.isEmpty()) return std::nullopt;
  Image img;
  img.mime = best->value("mimeType").value<TagLib::String>().to8Bit(true);
  img.data.assign(data.data(), data.size());
  if (img.mime.empty()) img.mime = "image/jpeg";
  return img;
}

std::optional<Image> Artwork::resize(const Image& in, int size) {
  int w = 0, h = 0, comp = 0;
  auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(in.data.data()),
                                       static_cast<int>(in.data.size()), &w, &h, &comp, 3);
  if (!pixels) return std::nullopt;
  if (std::max(w, h) <= size) {
    stbi_image_free(pixels);
    return in;
  }
  int nw = w >= h ? size : std::max(1, w * size / h);
  int nh = h > w ? size : std::max(1, h * size / w);
  std::vector<unsigned char> out(static_cast<size_t>(nw) * nh * 3);
  auto ok = stbir_resize_uint8_srgb(pixels, w, h, 0, out.data(), nw, nh, 0, STBIR_RGB);
  stbi_image_free(pixels);
  if (!ok) return std::nullopt;
  Image img{"image/jpeg", {}};
  stbi_write_jpg_to_func(
      [](void* ctx, void* data, int n) {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(n));
      },
      &img.data, nw, nh, 3, out.data(), 85);
  return img;
}

std::optional<fs::path> Artwork::folder_image(const fs::path& dir) const {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return std::nullopt;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file(ec) && !mime_for(e.path()).empty()) files.push_back(e.path());
  for (const auto& pattern : cfg_.artwork_album_files) {
    // Patterns look like "cover.*": match the stem case-insensitively.
    auto stem = pattern.ends_with(".*") ? pattern.substr(0, pattern.size() - 2) : pattern;
    for (const auto& f : files)
      if (iequals_ascii(f.stem().string(), stem) || iequals_ascii(f.filename().string(), pattern)) return f;
  }
  return std::nullopt;
}

std::optional<Image> Artwork::finish(Image img, const std::string& source_key, int size) {
  if (size <= 0) return img;
  auto digest = crypto::sha256(source_key);
  auto name = crypto::hex(digest.data(), 16) + "_" + std::to_string(size) + ".jpg";
  auto cached = cache_dir_ / name;
  if (auto data = read_file(cached)) return Image{"image/jpeg", std::move(*data)};
  auto small = resize(img, size);
  if (!small) return img;  // undecodable: send the original
  std::error_code ec;
  fs::create_directories(cache_dir_, ec);
  auto tmp = cached;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(small->data.data(), static_cast<std::streamsize>(small->data.size()));
  }
  fs::rename(tmp, cached, ec);
  if (++writes_since_evict_ >= 50) {
    writes_since_evict_ = 0;
    evict_if_needed();
  }
  return small;
}

void Artwork::evict_if_needed() {
  const uintmax_t limit = static_cast<uintmax_t>(cfg_.artwork_cache_max_mb) * 1024 * 1024;
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  uintmax_t total = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(cache_dir_, ec)) {
    if (!e.is_regular_file(ec)) continue;
    total += e.file_size(ec);
    files.emplace_back(e.last_write_time(ec), e.path());
  }
  if (total <= limit) return;
  std::sort(files.begin(), files.end());
  for (const auto& [t, p] : files) {
    if (total <= limit * 9 / 10) break;
    auto sz = fs::file_size(p, ec);
    if (fs::remove(p, ec)) total -= sz;
  }
}

std::optional<Image> Artwork::artist(db::Connection& db, const std::string& artist_id, int size) {
  auto st = db.prepare("SELECT mime, data, updated_at FROM artist_image WHERE artist_id = ?");
  st.bind(1, artist_id);
  if (!st.step()) return std::nullopt;
  const auto bytes = st.blob(1);
  Image img{st.text(0), std::string(bytes.begin(), bytes.end())};
  return finish(std::move(img), "artist|" + artist_id + "|" + std::to_string(st.int64(2)), size);
}

std::optional<Image> Artwork::album(db::Connection& db, const std::string& album_id, int size) {
  auto st = db.prepare(
      "SELECT l.path, al.dir_path FROM album al JOIN library l ON l.id = al.library_id WHERE al.id = ?");
  st.bind(1, album_id);
  if (!st.step()) return std::nullopt;
  // A cover chosen by hand (PATCH /ext/albums/{id}: cover) comes first.
  if (auto chosen = db.scalar_text(
          "SELECT value FROM field_override WHERE entity_type = 'album' AND entity_id = ? AND field = 'cover'", album_id)) {
    if (auto f = library_image(db, album_id, *chosen)) {
      if (auto data = read_file(*f))
        return finish({mime_for(*f), std::move(*data)}, f->string() + "|" + std::to_string(mtime_of(*f)), size);
    }
  }
  if (!st.is_null(1)) {
    if (auto f = folder_image(fs::path(st.text(0)) / st.text(1))) {
      if (auto data = read_file(*f))
        return finish({mime_for(*f), std::move(*data)}, f->string() + "|" + std::to_string(mtime_of(*f)), size);
    }
  }
  auto first = db.scalar_text(
      "SELECT id FROM track WHERE album_id = ? AND missing = 0 AND has_embedded_art = 1 "
      "ORDER BY COALESCE(disc_number, 0), COALESCE(track_number, 0), path LIMIT 1",
      album_id);
  if (!first) {
    // A virtual album has no songs of its own: the cover of its first member.
    auto member = db.scalar_text(
        "SELECT t.id FROM album_track at JOIN track t ON t.id = at.track_id AND t.missing = 0 "
        "WHERE at.album_id = ? AND at.excluded = 0 "
        "ORDER BY COALESCE(at.disc_number, 0), COALESCE(at.track_number, 0), t.path LIMIT 1",
        album_id);
    return member ? track(db, *member, size) : std::nullopt;
  }
  auto file = track_file(db, *first);
  if (!file) return std::nullopt;
  auto img = embedded(*file);
  if (!img) return std::nullopt;
  return finish(std::move(*img), file->string() + "|" + std::to_string(mtime_of(*file)), size);
}

std::optional<Image> Artwork::track(db::Connection& db, const std::string& track_id, int size) {
  if (auto file = track_file(db, track_id)) {
    if (auto img = embedded(*file))
      return finish(std::move(*img), file->string() + "|" + std::to_string(mtime_of(*file)), size);
  }
  auto album = db.scalar_text("SELECT album_id FROM track WHERE id = ?", track_id);
  if (!album) return std::nullopt;
  return this->album(db, *album, size);
}

}  // namespace eos::media
