#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace eos::ids {

// Encodes bytes (big-endian unsigned integer) in base62 with a fixed width, so that
// equal-length inputs always give equal-length outputs. 16 bytes -> 22 characters.
std::string base62(std::span<const uint8_t> bytes);

// A random 128-bit id, 22 base62 characters.
std::string random_id();

// A deterministic 128-bit id: the first 16 bytes of SHA-256(input), in base62.
std::string hash_id(std::string_view input);

// Builds the input for hash_id from parts joined with the unit separator 0x1F.
std::string join_key(std::initializer_list<std::string_view> parts);

}  // namespace eos::ids
