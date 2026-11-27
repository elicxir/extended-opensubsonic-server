#include "auth/users.h"

#include <cctype>
#include <format>
#include <stdexcept>

#include "core/ids.h"

namespace eos::auth {
namespace {

std::string aad_for(int64_t user_id) { return std::format("eos-user:{}", user_id); }

std::array<uint8_t, 32> key_hash(std::string_view key) { return crypto::sha256(key); }

User row_to_user(db::Statement& st) {
  return User{st.int64(0), st.text(1), st.int64(2) != 0, st.int64(3)};
}

}  // namespace

bool valid_username(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (unsigned char c : name)
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
  return true;
}

User create_user(db::Connection& conn, std::string_view username, std::string_view password,
                 bool is_admin, const crypto::Bytes& secret_key) {
  if (!valid_username(username)) throw std::invalid_argument("invalid username");
  if (password.empty()) throw std::invalid_argument("empty password");
  if (find_user(conn, username)) throw std::invalid_argument("user already exists");
  db::Transaction tx(conn);
  auto now = db::now_ms();
  conn.run("INSERT INTO user (username, is_admin, created_at) VALUES (?, ?, ?)", username,
           is_admin, now);
  auto id = conn.last_insert_rowid();
  set_password(conn, id, password, secret_key);
  tx.commit();
  return User{id, std::string(username), is_admin, now};
}

void set_password(db::Connection& conn, int64_t user_id, std::string_view password,
                  const crypto::Bytes& secret_key) {
  auto hash = crypto::argon2id_hash(password);
  auto enc = crypto::aes_gcm_encrypt(secret_key, password, aad_for(user_id));
  auto st = conn.prepare("UPDATE user SET password_hash = ?, password_enc = ? WHERE id = ?");
  st.bind(1, hash).bind_blob(2, enc.data(), enc.size()).bind(3, user_id);
  st.run();
}

std::optional<User> find_user(db::Connection& conn, std::string_view username) {
  auto st = conn.prepare("SELECT id, username, is_admin, created_at FROM user WHERE username = ?");
  st.bind(1, username);
  if (!st.step()) return std::nullopt;
  return row_to_user(st);
}

std::vector<User> list_users(db::Connection& conn) {
  std::vector<User> out;
  auto st = conn.prepare("SELECT id, username, is_admin, created_at FROM user ORDER BY username");
  while (st.step()) out.push_back(row_to_user(st));
  return out;
}

bool verify_password(db::Connection& conn, int64_t user_id, std::string_view password) {
  auto hash = conn.scalar_text("SELECT password_hash FROM user WHERE id = ?", user_id);
  return hash && crypto::argon2id_verify(password, *hash);
}

std::optional<std::string> decrypt_password(db::Connection& conn, int64_t user_id,
                                            const crypto::Bytes& secret_key) {
  auto st = conn.prepare("SELECT password_enc FROM user WHERE id = ?");
  st.bind(1, user_id);
  if (!st.step() || st.is_null(0)) return std::nullopt;
  return crypto::aes_gcm_decrypt(secret_key, st.blob(0), aad_for(user_id));
}

NewApiKey create_api_key(db::Connection& conn, int64_t user_id, std::string_view device_name) {
  if (device_name.empty()) throw std::invalid_argument("device name is required");
  auto raw = crypto::random_bytes(32);
  NewApiKey out{ids::random_id(), "eos_" + ids::base62(raw)};
  auto hash = key_hash(out.key);
  auto st = conn.prepare(
      "INSERT INTO api_key (id, user_id, device_name, key_hash, created_at) VALUES (?, ?, ?, ?, ?)");
  st.bind(1, out.id).bind(2, user_id).bind(3, device_name).bind_blob(4, hash.data(), hash.size());
  st.bind(5, db::now_ms());
  st.run();
  return out;
}

std::vector<ApiKeyInfo> list_api_keys(db::Connection& conn, int64_t user_id) {
  std::vector<ApiKeyInfo> out;
  auto st = conn.prepare(
      "SELECT id, user_id, device_name, created_at, last_used_at, revoked_at FROM api_key "
      "WHERE user_id = ? ORDER BY created_at");
  st.bind(1, user_id);
  while (st.step())
    out.push_back({st.text(0), st.int64(1), st.text(2), st.int64(3), st.opt_int64(4), st.opt_int64(5)});
  return out;
}

bool revoke_api_key(db::Connection& conn, std::string_view key_id) {
  conn.run("UPDATE api_key SET revoked_at = ? WHERE id = ? AND revoked_at IS NULL", db::now_ms(),
           key_id);
  return conn.changes() > 0;
}

std::optional<int64_t> lookup_api_key(db::Connection& conn, std::string_view key) {
  auto hash = key_hash(key);
  auto st = conn.prepare("SELECT user_id FROM api_key WHERE key_hash = ? AND revoked_at IS NULL");
  st.bind_blob(1, hash.data(), hash.size());
  if (!st.step()) return std::nullopt;
  return st.int64(0);
}

}  // namespace eos::auth
