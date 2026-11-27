#include "core/crypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sys/stat.h>

#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>

#include "core/strings.h"

namespace eos::crypto {
namespace {

constexpr uint32_t kArgonMemKiB = 65536;
constexpr uint32_t kArgonIter = 3;
constexpr uint32_t kArgonLanes = 1;
constexpr size_t kArgonSaltLen = 16;
constexpr size_t kArgonHashLen = 32;
constexpr uint8_t kBlobVersion = 1;

Bytes argon2id_raw(std::string_view password, const Bytes& salt, uint32_t mem, uint32_t iter,
                   uint32_t lanes, size_t out_len) {
  std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr),
                                                        &EVP_KDF_free);
  if (!kdf) throw std::runtime_error("OpenSSL has no ARGON2ID KDF (need OpenSSL 3.2 or later)");
  std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> ctx(EVP_KDF_CTX_new(kdf.get()),
                                                                &EVP_KDF_CTX_free);
  uint32_t threads = 1;
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                        const_cast<char*>(password.data()), password.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                        const_cast<uint8_t*>(salt.data()), salt.size()),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iter),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &mem),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
      OSSL_PARAM_construct_end()};
  Bytes out(out_len);
  if (EVP_KDF_derive(ctx.get(), out.data(), out.size(), params) != 1)
    throw std::runtime_error("Argon2id derivation failed");
  return out;
}

}  // namespace

Bytes random_bytes(size_t n) {
  Bytes out(n);
  if (n > 0 && RAND_bytes(out.data(), static_cast<int>(n)) != 1)
    throw std::runtime_error("RAND_bytes failed");
  return out;
}

std::array<uint8_t, 32> sha256(std::string_view data) {
  std::array<uint8_t, 32> out{};
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out.data());
  return out;
}

std::string hex(const uint8_t* data, size_t n) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0xF];
  }
  return out;
}

std::string base64(const Bytes& data) {
  std::string out(4 * ((data.size() + 2) / 3), '\0');
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                          static_cast<int>(data.size()));
  out.resize(static_cast<size_t>(n));
  return out;
}

std::optional<Bytes> base64_decode(std::string_view text) {
  if (text.size() % 4 != 0) return std::nullopt;
  Bytes out(3 * text.size() / 4);
  int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(text.data()),
                          static_cast<int>(text.size()));
  if (n < 0) return std::nullopt;
  size_t pad = 0;
  if (!text.empty() && text.back() == '=') ++pad;
  if (text.size() > 1 && text[text.size() - 2] == '=') ++pad;
  out.resize(static_cast<size_t>(n) - pad);
  return out;
}

std::string argon2id_hash(std::string_view password) {
  auto salt = random_bytes(kArgonSaltLen);
  auto hash = argon2id_raw(password, salt, kArgonMemKiB, kArgonIter, kArgonLanes, kArgonHashLen);
  return std::format("$argon2id$v=19$m={},t={},p={}${}${}", kArgonMemKiB, kArgonIter, kArgonLanes,
                     base64(salt), base64(hash));
}

bool argon2id_verify(std::string_view password, std::string_view encoded) {
  // $argon2id$v=19$m=..,t=..,p=..$salt$hash  -> ["", "argon2id", "v=19", "m=..", "salt", "hash"]
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= encoded.size(); ++i) {
    if (i == encoded.size() || encoded[i] == '$') {
      parts.emplace_back(encoded.substr(start, i - start));
      start = i + 1;
    }
  }
  if (parts.size() != 6 || parts[1] != "argon2id" || parts[2] != "v=19") return false;
  uint32_t m = 0, t = 0, p = 0;
  if (std::sscanf(parts[3].c_str(), "m=%u,t=%u,p=%u", &m, &t, &p) != 3) return false;
  auto salt = base64_decode(parts[4]);
  auto expected = base64_decode(parts[5]);
  if (!salt || !expected || expected->empty()) return false;
  auto actual = argon2id_raw(password, *salt, m, t, p, expected->size());
  return equal(std::string_view(reinterpret_cast<const char*>(actual.data()), actual.size()),
               std::string_view(reinterpret_cast<const char*>(expected->data()), expected->size()));
}

Bytes aes_gcm_encrypt(const Bytes& key, std::string_view plaintext, std::string_view aad) {
  if (key.size() != 32) throw std::invalid_argument("AES-256-GCM needs a 32-byte key");
  // Layout: version(1) || nonce(12) || ciphertext(n) || tag(16), written in place.
  Bytes out(1 + 12 + plaintext.size() + 16);
  out[0] = kBlobVersion;
  uint8_t* nonce = out.data() + 1;
  uint8_t* cipher = out.data() + 13;
  uint8_t* tag = cipher + plaintext.size();
  if (RAND_bytes(nonce, 12) != 1) throw std::runtime_error("RAND_bytes failed");
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                      &EVP_CIPHER_CTX_free);
  int len = 0;
  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) != 1)
    throw std::runtime_error("EVP_EncryptInit_ex failed");
  if (!aad.empty() &&
      EVP_EncryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(aad.data()),
                        static_cast<int>(aad.size())) != 1)
    throw std::runtime_error("EVP_EncryptUpdate(aad) failed");
  if (EVP_EncryptUpdate(ctx.get(), cipher, &len, reinterpret_cast<const unsigned char*>(plaintext.data()),
                        static_cast<int>(plaintext.size())) != 1)
    throw std::runtime_error("EVP_EncryptUpdate failed");
  int final_len = 0;  // GCM is a stream mode: nothing is held back
  if (EVP_EncryptFinal_ex(ctx.get(), cipher + len, &final_len) != 1)
    throw std::runtime_error("EVP_EncryptFinal_ex failed");
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
    throw std::runtime_error("GCM get tag failed");
  return out;
}

std::optional<std::string> aes_gcm_decrypt(const Bytes& key, const Bytes& blob,
                                           std::string_view aad) {
  if (key.size() != 32 || blob.size() < 1 + 12 + 16 || blob[0] != kBlobVersion) return std::nullopt;
  const uint8_t* nonce = blob.data() + 1;
  const uint8_t* cipher = blob.data() + 13;
  size_t cipher_len = blob.size() - 13 - 16;
  const uint8_t* tag = blob.data() + blob.size() - 16;
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                      &EVP_CIPHER_CTX_free);
  int len = 0;
  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) != 1)
    return std::nullopt;
  if (!aad.empty() &&
      EVP_DecryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(aad.data()),
                        static_cast<int>(aad.size())) != 1)
    return std::nullopt;
  std::string plain(cipher_len + 16, '\0');
  if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &len, cipher,
                        static_cast<int>(cipher_len)) != 1)
    return std::nullopt;
  int total = len;
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) != 1)
    return std::nullopt;
  if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()) + total, &len) != 1)
    return std::nullopt;
  total += len;
  plain.resize(static_cast<size_t>(total));
  return plain;
}

Bytes load_or_create_secret_key(const std::filesystem::path& path) {
  namespace fs = std::filesystem;
  if (fs::exists(path)) {
    std::ifstream in(path, std::ios::binary);
    Bytes key((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (key.size() != 32)
      throw std::runtime_error(std::format("secret key {} must be exactly 32 bytes", path.string()));
    return key;
  }
  if (path.has_parent_path()) fs::create_directories(path.parent_path());
  auto key = random_bytes(32);
  auto old_mask = ::umask(077);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(key.data()), static_cast<std::streamsize>(key.size()));
    if (!out) {
      ::umask(old_mask);
      throw std::runtime_error(std::format("cannot write secret key {}", path.string()));
    }
  }
  ::umask(old_mask);
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  return key;
}

bool equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
}

}  // namespace eos::crypto
