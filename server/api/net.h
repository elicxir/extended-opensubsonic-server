#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eos::api {

// An IPv4 or IPv6 network. IPv4 addresses are stored as IPv4-mapped IPv6.
struct Cidr {
  std::array<uint8_t, 16> addr{};
  int prefix = 128;

  static std::optional<Cidr> parse(std::string_view text);  // "10.0.0.0/8", "::1", "1.2.3.4"
  bool contains(const std::array<uint8_t, 16>& ip) const;
};

std::optional<std::array<uint8_t, 16>> parse_ip(std::string_view text);

class IpSet {
 public:
  IpSet() = default;
  explicit IpSet(const std::vector<std::string>& cidrs);  // invalid entries are logged and skipped
  bool contains(std::string_view ip) const;

 private:
  std::vector<Cidr> nets_;
};

// The client address: the TCP peer, or - when the peer is a trusted proxy - the last
// X-Forwarded-For entry that is not itself a trusted proxy.
std::string client_ip(std::string_view peer, const std::optional<std::string>& forwarded_for,
                      const IpSet& trusted_proxies);

}  // namespace eos::api
