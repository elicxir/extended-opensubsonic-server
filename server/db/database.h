#pragma once

#include <sqlite3.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace eos::db {

struct DbError : std::runtime_error {
  int code;
  DbError(int c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

class Statement {
 public:
  Statement(sqlite3* db, std::string_view sql);
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&&) = delete;

  // Binding, 1-based like SQLite.
  Statement& bind(int idx, std::nullptr_t);
  Statement& bind(int idx, int64_t v);
  Statement& bind(int idx, int v) { return bind(idx, static_cast<int64_t>(v)); }
  Statement& bind(int idx, bool v) { return bind(idx, static_cast<int64_t>(v ? 1 : 0)); }
  Statement& bind(int idx, double v);
  Statement& bind(int idx, std::string_view v);
  Statement& bind(int idx, const std::string& v) { return bind(idx, std::string_view(v)); }
  Statement& bind(int idx, const char* v) { return bind(idx, std::string_view(v)); }
  Statement& bind_blob(int idx, const void* data, size_t n);
  template <class T>
  Statement& bind(int idx, const std::optional<T>& v) {
    return v ? bind(idx, *v) : bind(idx, nullptr);
  }

  // Binds all arguments from index 1.
  template <class... Args>
  Statement& bind_all(const Args&... args) {
    int i = 0;
    (bind(++i, args), ...);
    return *this;
  }

  // Returns true while a row is available. Throws on error.
  bool step();
  // Runs to completion (for statements that return no rows).
  void run();
  void reset();

  bool is_null(int col) const;
  int64_t int64(int col) const;
  std::optional<int64_t> opt_int64(int col) const;
  double real(int col) const;
  std::string text(int col) const;
  std::optional<std::string> opt_text(int col) const;
  std::vector<uint8_t> blob(int col) const;

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

class Connection {
 public:
  enum class Mode { read_write, read_only };
  explicit Connection(const std::filesystem::path& path, Mode mode = Mode::read_write);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void exec(std::string_view sql);
  Statement prepare(std::string_view sql) { return Statement(db_, sql); }

  template <class... Args>
  void run(std::string_view sql, const Args&... args) {
    auto st = prepare(sql);
    st.bind_all(args...);
    st.run();
  }

  // First column of the first row, or nullopt when there is no row.
  template <class... Args>
  std::optional<int64_t> scalar_int(std::string_view sql, const Args&... args) {
    auto st = prepare(sql);
    st.bind_all(args...);
    if (!st.step() || st.is_null(0)) return std::nullopt;
    return st.int64(0);
  }
  template <class... Args>
  std::optional<std::string> scalar_text(std::string_view sql, const Args&... args) {
    auto st = prepare(sql);
    st.bind_all(args...);
    if (!st.step() || st.is_null(0)) return std::nullopt;
    return st.text(0);
  }

  int64_t changes() const;
  int64_t last_insert_rowid() const;
  sqlite3* raw() const { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back when destroyed without commit().
class Transaction {
 public:
  explicit Transaction(Connection& c);
  ~Transaction();
  void commit();

 private:
  Connection& conn_;
  bool done_ = false;
};

// A fixed set of read-only connections. acquire() blocks until one is free.
class ReadPool {
 public:
  ReadPool(const std::filesystem::path& path, size_t size);

  class Lease {
   public:
    Lease(ReadPool& pool, std::unique_ptr<Connection> conn) : pool_(&pool), conn_(std::move(conn)) {}
    Lease(Lease&&) = default;
    ~Lease();
    Connection& operator*() { return *conn_; }
    Connection* operator->() { return conn_.get(); }

   private:
    ReadPool* pool_;
    std::unique_ptr<Connection> conn_;
  };

  Lease acquire();

 private:
  void release(std::unique_ptr<Connection> conn);
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Connection>> free_;
};

int64_t now_ms();

}  // namespace eos::db
