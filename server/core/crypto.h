#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eos::crypto {

using Bytes = std::vector<uint8_t>;

Bytes random_bytes(size_t n);
std::array<uint8_t, 32> sha256(std::string_view data);
std::string hex(const uint8_t* data, size_t n);
std::string base64(const Bytes& data);
std::optional<Bytes> base64_decode(std::string_view text);

// Password hashing with Argon2id (OpenSSL >= 3.2). The result is a PHC-style string:
// $argon2id$v=19$m=65536,t=3,p=1$<salt b64>$<hash b64>
std::string argon2id_hash(std::string_view password);
bool argon2id_verify(std::string_view password, std::string_view encoded);

// AES-256-GCM. Output layout: version(1) || nonce(12) || ciphertext || tag(16).
// `aad` is authenticated but not stored (we pass the user id).
Bytes aes_gcm_encrypt(const Bytes& key, std::string_view plaintext, std::string_view aad);
std::optional<std::string> aes_gcm_decrypt(const Bytes& key, const Bytes& blob, std::string_view aad);

// Loads the 32-byte key at `path`, creating it (mode 0600) when missing.
Bytes load_or_create_secret_key(const std::filesystem::path& path);

// Constant-time comparison.
bool equal(std::string_view a, std::string_view b);

}  // namespace eos::crypto
