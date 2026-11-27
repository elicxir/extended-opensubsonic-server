#include "scanner/tag_reader.h"

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacproperties.h>
#include <taglib/tpropertymap.h>

#include "core/crypto.h"
#include "core/strings.h"

namespace eos::scanner {

std::optional<RawFile> read_file(const std::filesystem::path& path, std::string& error) {
  TagLib::FileRef ref(path.c_str(), true, TagLib::AudioProperties::Average);
  if (ref.isNull() || !ref.file() || !ref.file()->isValid()) {
    error = "unreadable or unsupported file";
    return std::nullopt;
  }

  RawFile out;
  const auto props = ref.properties();
  for (const auto& [key, values] : props) {
    auto k = to_upper_ascii(key.to8Bit(true));
    auto& dst = out.tags[k];
    for (const auto& v : values) dst.push_back(v.to8Bit(true));
  }

  if (auto ap = ref.audioProperties()) {
    out.duration_ms = ap->lengthInMilliseconds();
    out.bit_rate = ap->bitrate();
    out.sample_rate = ap->sampleRate();
    out.channels = ap->channels();
  }

  if (auto flac = dynamic_cast<TagLib::FLAC::File*>(ref.file())) {
    if (auto fp = flac->audioProperties()) {
      out.bit_depth = fp->bitsPerSample();
      auto sig = fp->signature();
      bool all_zero = true;
      for (auto c : sig) all_zero = all_zero && c == 0;
      if (sig.size() == 16 && !all_zero)
        out.audio_md5 = crypto::hex(reinterpret_cast<const uint8_t*>(sig.data()), sig.size());
    }
    out.has_picture = !flac->pictureList().isEmpty();
  } else {
    out.has_picture = ref.complexPropertyKeys().contains("PICTURE");
  }
  return out;
}

}  // namespace eos::scanner
