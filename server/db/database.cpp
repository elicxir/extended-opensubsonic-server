#include "db/database.h"

#include <chrono>
#include <format>

namespace eos::db {
namespace {
[[noreturn]] void fail(sqlite3* db, int rc, std::string_view what) {
  throw DbError(rc, std::format("{}: {}", what, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)));
}
}  // namespace

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---- Statement ----

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
  int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
  if (rc != SQLITE_OK) fail(db, rc, std::format("prepare [{}]", sql.substr(0, 80)));
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
  other.stmt_ = nullptr;
}

Statement& Statement::bind(int idx, std::nullptr_t) {
  if (int rc = sqlite3_bind_null(stmt_, idx); rc != SQLITE_OK) fail(db_, rc, "bind null");
  return *this;
}
Statement& Statement::bind(int idx, int64_t v) {
  if (int rc = sqlite3_bind_int64(stmt_, idx, v); rc != SQLITE_OK) fail(db_, rc, "bind int");
  return *this;
}
Statement& Statement::bind(int idx, double v) {
  if (int rc = sqlite3_bind_double(stmt_, idx, v); rc != SQLITE_OK) fail(db_, rc, "bind double");
  return *this;
}
Statement& Statement::bind(int idx, std::string_view v) {
  int rc = sqlite3_bind_text64(stmt_, idx, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
  if (rc != SQLITE_OK) fail(db_, rc, "bind text");
  return *this;
}
Statement& Statement::bind_blob(int idx, const void* data, size_t n) {
  int rc = sqlite3_bind_blob64(stmt_, idx, data, n, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) fail(db_, rc, "bind blob");
  return *this;
}

bool Statement::step() {
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  fail(db_, rc, "step");
}

void Statement::run() {
  while (step()) {
  }
}

void Statement::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

bool Statement::is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
int64_t Statement::int64(int col) const { return sqlite3_column_int64(stmt_, col); }
std::optional<int64_t> Statement::opt_int64(int col) const {
  if (is_null(col)) return std::nullopt;
  return int64(col);
}
double Statement::real(int col) const { return sqlite3_column_double(stmt_, col); }
std::string Statement::text(int col) const {
  auto p = sqlite3_column_text(stmt_, col);
  if (!p) return {};
  return std::string(reinterpret_cast<const char*>(p),
                     static_cast<size_t>(sqlite3_column_bytes(stmt_, col)));
}
std::optional<std::string> Statement::opt_text(int col) const {
  if (is_null(col)) return std::nullopt;
  return text(col);
}
std::vector<uint8_t> Statement::blob(int col) const {
  auto p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, col));
  auto n = static_cast<size_t>(sqlite3_column_bytes(stmt_, col));
  return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>{};
}

// ---- Connection ----

Connection::Connection(const std::filesystem::path& path, Mode mode) {
  int flags = SQLITE_OPEN_NOMUTEX;
  flags |= (mode == Mode::read_only) ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
    sqlite3_close(db_);
    db_ = nullptr;
    throw DbError(rc, std::format("open {}: {}", path.string(), msg));
  }
  sqlite3_busy_timeout(db_, 5000);
  exec("PRAGMA foreign_keys = ON");
  if (mode == Mode::read_write) {
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous = NORMAL");
  }
}

Connection::~Connection() { sqlite3_close_v2(db_); }

void Connection::exec(std::string_view sql) {
  std::string s(sql);
  char* err = nullptr;
  int rc = sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : sqlite3_errstr(rc);
    sqlite3_free(err);
    throw DbError(rc, "exec: " + msg);
  }
}

int64_t Connection::changes() const { return sqlite3_changes64(db_); }
int64_t Connection::last_insert_rowid() const { return sqlite3_last_insert_rowid(db_); }

// ---- Transaction ----

Transaction::Transaction(Connection& c) : conn_(c) { conn_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
  if (!done_) {
    try {
      conn_.exec("ROLLBACK");
    } catch (...) {
    }
  }
}

void Transaction::commit() {
  conn_.exec("COMMIT");
  done_ = true;
}

// ---- ReadPool ----

ReadPool::ReadPool(const std::filesystem::path& path, size_t size) {
  for (size_t i = 0; i < size; ++i)
    free_.push_back(std::make_unique<Connection>(path, Connection::Mode::read_only));
}

ReadPool::Lease ReadPool::acquire() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [&] { return !free_.empty(); });
  auto conn = std::move(free_.back());
  free_.pop_back();
  return Lease(*this, std::move(conn));
}

void ReadPool::release(std::unique_ptr<Connection> conn) {
  {
    std::lock_guard lock(mutex_);
    free_.push_back(std::move(conn));
  }
  cv_.notify_one();
}

ReadPool::Lease::~Lease() {
  if (conn_) pool_->release(std::move(conn_));
}

}  // namespace eos::db
