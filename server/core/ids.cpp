#include "core/ids.h"

#include <cmath>
#include <vector>

#include "core/crypto.h"

namespace eos::ids {
namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
}

std::string base62(std::span<const uint8_t> bytes) {
  // Fixed width: smallest w with 62^w >= 256^n.
  const size_t width =
      static_cast<size_t>(std::ceil(static_cast<double>(bytes.size()) * 8.0 / std::log2(62.0)));
  std::vector<uint8_t> num(bytes.begin(), bytes.end());
  std::string out(width, '0');
  for (size_t pos = width; pos-- > 0;) {
    uint32_t rem = 0;
    for (auto& b : num) {
      uint32_t cur = (rem << 8) | b;
      b = static_cast<uint8_t>(cur / 62);
      rem = cur % 62;
    }
    out[pos] = kAlphabet[rem];
  }
  return out;
}

std::string random_id() {
  auto bytes = crypto::random_bytes(16);
  return base62(bytes);
}

std::string hash_id(std::string_view input) {
  auto digest = crypto::sha256(input);
  return base62(std::span<const uint8_t>(digest.data(), 16));
}

std::string join_key(std::initializer_list<std::string_view> parts) {
  std::string out;
  bool first = true;
  for (auto p : parts) {
    if (!first) out.push_back('\x1f');
    out.append(p);
    first = false;
  }
  return out;
}

}  // namespace eos::ids
