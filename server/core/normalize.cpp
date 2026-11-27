#include "core/normalize.h"

#include <utf8proc.h>

#include <cstdlib>
#include <memory>

namespace eos {
namespace {

std::string latin1_to_utf8(std::string_view in) {
  std::string out;
  for (unsigned char c : in) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

bool is_valid_utf8(std::string_view s) {
  auto p = reinterpret_cast<const utf8proc_uint8_t*>(s.data());
  utf8proc_ssize_t len = static_cast<utf8proc_ssize_t>(s.size());
  utf8proc_ssize_t pos = 0;
  while (pos < len) {
    utf8proc_int32_t cp;
    auto n = utf8proc_iterate(p + pos, len - pos, &cp);
    if (n <= 0) return false;
    pos += n;
  }
  return true;
}

bool dropped(utf8proc_int32_t cp) {
  switch (utf8proc_category(cp)) {
    case UTF8PROC_CATEGORY_PC: case UTF8PROC_CATEGORY_PD: case UTF8PROC_CATEGORY_PS:
    case UTF8PROC_CATEGORY_PE: case UTF8PROC_CATEGORY_PI: case UTF8PROC_CATEGORY_PF:
    case UTF8PROC_CATEGORY_PO:
    case UTF8PROC_CATEGORY_SM: case UTF8PROC_CATEGORY_SC: case UTF8PROC_CATEGORY_SK:
    case UTF8PROC_CATEGORY_SO:
    case UTF8PROC_CATEGORY_ZS: case UTF8PROC_CATEGORY_ZL: case UTF8PROC_CATEGORY_ZP:
    case UTF8PROC_CATEGORY_CC: case UTF8PROC_CATEGORY_CF:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string normalize(std::string_view input) {
  std::string source = is_valid_utf8(input) ? std::string(input) : latin1_to_utf8(input);

  std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> folded(
      utf8proc_NFKC_Casefold(reinterpret_cast<const utf8proc_uint8_t*>(source.c_str())),
      &std::free);
  if (!folded) return {};

  std::string out;
  const utf8proc_uint8_t* p = folded.get();
  utf8proc_ssize_t pos = 0;
  utf8proc_int32_t cp;
  utf8proc_uint8_t buf[4];
  while (true) {
    auto n = utf8proc_iterate(p + pos, -1, &cp);
    if (n <= 0 || cp == 0) break;
    pos += n;
    if (dropped(cp)) continue;
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;  // katakana -> hiragana
    auto len = utf8proc_encode_char(cp, buf);
    out.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(len));
  }
  return out;
}

}  // namespace eos
