#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/crypto.h"
#include "db/database.h"

namespace eos::auth {

struct User {
  int64_t id;
  std::string username;
  bool is_admin;
  int64_t created_at;
};

struct ApiKeyInfo {
  std::string id;
  int64_t user_id;
  std::string device_name;
  int64_t created_at;
  std::optional<int64_t> last_used_at;
  std::optional<int64_t> revoked_at;
};

struct NewApiKey {
  std::string id;
  std::string key;  // shown to the user once; only its SHA-256 is stored
};

// Creates a user. The password is stored as an Argon2id hash and, for token
// authentication, encrypted with `secret_key` (AES-256-GCM, user id as AAD).
User create_user(db::Connection& conn, std::string_view username, std::string_view password,
                 bool is_admin, const crypto::Bytes& secret_key);

void set_password(db::Connection& conn, int64_t user_id, std::string_view password,
                  const crypto::Bytes& secret_key);

std::optional<User> find_user(db::Connection& conn, std::string_view username);
std::vector<User> list_users(db::Connection& conn);

bool verify_password(db::Connection& conn, int64_t user_id, std::string_view password);
// Recovers the password for Subsonic token auth (md5(password + salt)).
std::optional<std::string> decrypt_password(db::Connection& conn, int64_t user_id,
                                            const crypto::Bytes& secret_key);

NewApiKey create_api_key(db::Connection& conn, int64_t user_id, std::string_view device_name);
std::vector<ApiKeyInfo> list_api_keys(db::Connection& conn, int64_t user_id);
bool revoke_api_key(db::Connection& conn, std::string_view key_id);
// Returns the user id for a valid, unrevoked key.
std::optional<int64_t> lookup_api_key(db::Connection& conn, std::string_view key);

// Usernames: 1-64 characters from [A-Za-z0-9._-].
bool valid_username(std::string_view name);

}  // namespace eos::auth
