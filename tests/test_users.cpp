#include <gtest/gtest.h>

#include "auth/users.h"
#include "db/migrate.h"
#include "fixtures.h"

using namespace eos;

TEST(Users, CreateVerifyAndDecrypt) {
  test::TempDir dir;
  db::Connection c(dir.path() / "eos.db");
  db::migrate(c);
  auto key = crypto::random_bytes(32);
  auto u = auth::create_user(c, "alice", "pw-1", true, key);
  EXPECT_TRUE(u.is_admin);
  EXPECT_TRUE(auth::verify_password(c, u.id, "pw-1"));
  EXPECT_FALSE(auth::verify_password(c, u.id, "pw-2"));
  EXPECT_EQ(auth::decrypt_password(c, u.id, key), "pw-1");
  EXPECT_EQ(auth::decrypt_password(c, u.id, crypto::random_bytes(32)), std::nullopt);
  EXPECT_THROW(auth::create_user(c, "alice", "x", false, key), std::invalid_argument);
  EXPECT_THROW(auth::create_user(c, "bad name", "x", false, key), std::invalid_argument);
}

TEST(Users, ApiKeysAreStoredHashedAndRevocable) {
  test::TempDir dir;
  db::Connection c(dir.path() / "eos.db");
  db::migrate(c);
  auto u = auth::create_user(c, "bob", "pw", false, crypto::random_bytes(32));
  auto k = auth::create_api_key(c, u.id, "pi");
  EXPECT_TRUE(k.key.starts_with("eos_"));
  EXPECT_EQ(k.key.size(), 4u + 43u);
  EXPECT_FALSE(c.scalar_int("SELECT 1 FROM api_key WHERE key_hash = ?", k.key));  // not plaintext
  EXPECT_EQ(auth::lookup_api_key(c, k.key), u.id);
  EXPECT_EQ(auth::lookup_api_key(c, "eos_wrong"), std::nullopt);
  EXPECT_TRUE(auth::revoke_api_key(c, k.id));
  EXPECT_FALSE(auth::revoke_api_key(c, k.id));
  EXPECT_EQ(auth::lookup_api_key(c, k.key), std::nullopt);
}
