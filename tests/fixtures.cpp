#include "fixtures.h"

#include <stb/stb_image_write.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tpropertymap.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <random>
#include <stdexcept>

namespace eos::test {
namespace fs = std::filesystem;

TempDir::TempDir() {
  std::random_device rd;
  path_ = fs::temp_directory_path() / std::format("eos-test-{:08x}{:08x}", rd(), rd());
  fs::create_directories(path_);
}

TempDir::~TempDir() {
  std::error_code ec;
  fs::remove_all(path_, ec);
}

bool have_flac_encoder() { return std::system("flac --version > /dev/null 2>&1") == 0; }

namespace {
void put16(std::ofstream& o, uint16_t v) { o.put(char(v & 0xff)).put(char(v >> 8)); }
void put32(std::ofstream& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.put(char((v >> (8 * i)) & 0xff));
}

void write_wav(const fs::path& path, int freq_hz) {
  const uint32_t rate = 44100, samples = rate / 2;  // 0.5 s mono 16-bit
  std::ofstream o(path, std::ios::binary);
  o.write("RIFF", 4);
  put32(o, 36 + samples * 2);
  o.write("WAVEfmt ", 8);
  put32(o, 16);
  put16(o, 1);
  put16(o, 1);
  put32(o, rate);
  put32(o, rate * 2);
  put16(o, 2);
  put16(o, 16);
  o.write("data", 4);
  put32(o, samples * 2);
  for (uint32_t i = 0; i < samples; ++i) {
    auto s = static_cast<int16_t>(8000 * std::sin(2 * M_PI * freq_hz * i / rate));
    put16(o, static_cast<uint16_t>(s));
  }
}
}  // namespace

void retag_flac(const fs::path& path, const Tags& tags) {
  TagLib::FLAC::File f(path.c_str());
  if (!f.isValid()) throw std::runtime_error("cannot open " + path.string());
  TagLib::PropertyMap map;
  for (const auto& [k, values] : tags) {
    TagLib::StringList list;
    for (const auto& v : values) list.append(TagLib::String(v, TagLib::String::UTF8));
    map.insert(TagLib::String(k, TagLib::String::UTF8), list);
  }
  f.xiphComment(true);
  f.setProperties(map);
  if (!f.save()) throw std::runtime_error("cannot save " + path.string());
}

void make_flac(const fs::path& path, int freq_hz, const Tags& tags) {
  fs::create_directories(path.parent_path());
  auto wav = path;
  wav += ".wav";
  write_wav(wav, freq_hz);
  auto cmd = std::format("flac --silent --force -o '{}' '{}'", path.string(), wav.string());
  if (std::system(cmd.c_str()) != 0) throw std::runtime_error("flac encoder failed");
  fs::remove(wav);
  retag_flac(path, tags);
}

void add_flac_picture(const fs::path& path, const std::string& data, const std::string& mime) {
  TagLib::FLAC::File f(path.c_str());
  if (!f.isValid()) throw std::runtime_error("cannot open " + path.string());
  auto* pic = new TagLib::FLAC::Picture();
  pic->setType(TagLib::FLAC::Picture::FrontCover);
  pic->setMimeType(mime);
  pic->setData(TagLib::ByteVector(data.data(), static_cast<unsigned int>(data.size())));
  f.addPicture(pic);
  if (!f.save()) throw std::runtime_error("cannot save " + path.string());
}

std::string make_png(int w, int h) {
  std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3, 0);
  for (size_t i = 0; i < px.size(); i += 3) px[i] = 200;
  std::string out;
  stbi_write_png_to_func(
      [](void* ctx, void* data, int n) {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(n));
      },
      &out, w, h, 3, px.data(), w * 3);
  return out;
}

}  // namespace eos::test
