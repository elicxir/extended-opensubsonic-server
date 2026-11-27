#include <gtest/gtest.h>

#include <filesystem>

#include "core/crypto.h"
#include "fixtures.h"

using namespace eos;

TEST(Crypto, Argon2idRoundTrip) {
  auto h = crypto::argon2id_hash("correct horse");
  EXPECT_TRUE(h.starts_with("$argon2id$v=19$m=65536,t=3,p=1$"));
  EXPECT_TRUE(crypto::argon2id_verify("correct horse", h));
  EXPECT_FALSE(crypto::argon2id_verify("wrong", h));
  EXPECT_FALSE(crypto::argon2id_verify("correct horse", "$argon2id$garbage"));
}

TEST(Crypto, AesGcmRoundTripAndAad) {
  auto key = crypto::random_bytes(32);
  auto blob = crypto::aes_gcm_encrypt(key, "s3cret", "eos-user:1");
  EXPECT_EQ(crypto::aes_gcm_decrypt(key, blob, "eos-user:1"), "s3cret");
  EXPECT_EQ(crypto::aes_gcm_decrypt(key, blob, "eos-user:2"), std::nullopt);  // moved to another row
  blob[15] ^= 1;
  EXPECT_EQ(crypto::aes_gcm_decrypt(key, blob, "eos-user:1"), std::nullopt);  // tampered
}

TEST(Crypto, SecretKeyIsCreatedOnceWithOwnerOnlyMode) {
  test::TempDir dir;
  auto path = dir.path() / "sub" / "secret.key";
  auto k1 = crypto::load_or_create_secret_key(path);
  auto k2 = crypto::load_or_create_secret_key(path);
  EXPECT_EQ(k1, k2);
  EXPECT_EQ(k1.size(), 32u);
  auto perms = std::filesystem::status(path).permissions();
  EXPECT_EQ(perms & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

TEST(Crypto, Base64RoundTrip) {
  crypto::Bytes data = {0, 1, 2, 250, 251};
  EXPECT_EQ(crypto::base64_decode(crypto::base64(data)), data);
}
