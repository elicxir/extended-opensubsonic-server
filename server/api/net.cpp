#include "api/net.h"

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <cstring>

#include "core/log.h"
#include "core/strings.h"

namespace eos::api {

std::optional<std::array<uint8_t, 16>> parse_ip(std::string_view text) {
  std::string s = trim(text);
  if (s.size() > 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
  if (auto pct = s.find('%'); pct != std::string::npos) s.resize(pct);  // zone id
  std::array<uint8_t, 16> out{};
  in_addr v4{};
  if (inet_pton(AF_INET, s.c_str(), &v4) == 1) {
    out[10] = 0xff;
    out[11] = 0xff;
    std::memcpy(out.data() + 12, &v4, 4);
    return out;
  }
  in6_addr v6{};
  if (inet_pton(AF_INET6, s.c_str(), &v6) == 1) {
    std::memcpy(out.data(), &v6, 16);
    return out;
  }
  return std::nullopt;
}

std::optional<Cidr> Cidr::parse(std::string_view text) {
  auto t = trim(text);
  auto slash = t.find('/');
  auto ip = parse_ip(std::string_view(t).substr(0, slash));
  if (!ip) return std::nullopt;
  bool is_v4 = (*ip)[10] == 0xff && (*ip)[11] == 0xff &&
               std::all_of(ip->begin(), ip->begin() + 10, [](uint8_t b) { return b == 0; });
  Cidr c;
  c.addr = *ip;
  c.prefix = 128;
  if (slash != std::string::npos) {
    int p = -1;
    auto rest = std::string_view(t).substr(slash + 1);
    auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), p);
    if (ec != std::errc() || ptr != rest.data() + rest.size()) return std::nullopt;
    if (is_v4) {
      if (p < 0 || p > 32) return std::nullopt;
      p += 96;
    }
    if (p < 0 || p > 128) return std::nullopt;
    c.prefix = p;
  }
  return c;
}

bool Cidr::contains(const std::array<uint8_t, 16>& ip) const {
  int bits = prefix;
  for (int i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
    uint8_t mask = bits >= 8 ? 0xff : static_cast<uint8_t>(0xff << (8 - bits));
    if ((ip[i] & mask) != (addr[i] & mask)) return false;
  }
  return true;
}

IpSet::IpSet(const std::vector<std::string>& cidrs) {
  for (const auto& c : cidrs) {
    if (auto parsed = Cidr::parse(c)) nets_.push_back(*parsed);
    else log::warn("ignoring invalid address or network '{}'", c);
  }
}

bool IpSet::contains(std::string_view ip) const {
  auto parsed = parse_ip(ip);
  if (!parsed) return false;
  for (const auto& n : nets_)
    if (n.contains(*parsed)) return true;
  return false;
}

std::string client_ip(std::string_view peer, const std::optional<std::string>& forwarded_for,
                      const IpSet& trusted) {
  std::string ip(peer);
  if (!forwarded_for || !trusted.contains(ip)) return ip;
  auto hops = split_any(*forwarded_for, {","});
  for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
    if (!parse_ip(*it)) break;  // garbage: stop trusting the header
    ip = *it;
    if (!trusted.contains(ip)) break;
  }
  return ip;
}

}  // namespace eos::api
