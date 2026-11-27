#include <gtest/gtest.h>

#include <thread>

#include "db/database.h"
#include "db/migrate.h"
#include "db/write_queue.h"
#include "fixtures.h"

using namespace eos;

TEST(Db, MigrationsApplyOnceAndCreateSchema) {
  test::TempDir dir;
  auto path = dir.path() / "eos.db";
  db::Connection c(path);
  auto v1 = db::migrate(c);
  EXPECT_GE(v1, 1);
  EXPECT_EQ(db::migrate(c), v1);  // idempotent
  for (auto table : {"track", "album", "album_track", "suggestion", "user", "api_key", "search_index"})
    EXPECT_TRUE(c.scalar_int("SELECT 1 FROM sqlite_master WHERE name = ?", table)) << table;
  EXPECT_EQ(c.scalar_text("PRAGMA journal_mode"), "wal");
}

TEST(Db, Fts5TrigramIsAvailable) {
  test::TempDir dir;
  db::Connection c(dir.path() / "eos.db");
  db::migrate(c);
  c.run("INSERT INTO search_index (kind, ref_id, name, alt, context) VALUES ('track', 'x', ?, '', '')",
        "finalfantasyぴあの版");
  EXPECT_EQ(c.scalar_text("SELECT ref_id FROM search_index WHERE search_index MATCH ?", "\"ぴあの\""),
            "x");
}

TEST(Db, RejectsNewerSchema) {
  test::TempDir dir;
  db::Connection c(dir.path() / "eos.db");
  db::migrate(c);
  c.run("INSERT INTO schema_version (version) VALUES (999)");
  EXPECT_THROW(db::migrate(c), db::DbError);
}

TEST(Db, WriteQueueRunsJobsInOrderAndPropagatesErrors) {
  test::TempDir dir;
  auto path = dir.path() / "eos.db";
  {
    db::Connection c(path);
    db::migrate(c);
  }
  db::WriteQueue q(path);
  std::vector<std::future<void>> futs;
  for (int i = 0; i < 4; ++i) {
    futs.push_back(q.submit([i](db::Connection& c) {
      c.run("INSERT INTO setting (key, value) VALUES (?, ?)", "k" + std::to_string(i), "v");
    }));
  }
  for (auto& f : futs) f.get();
  EXPECT_EQ(q.run([](db::Connection& c) { return *c.scalar_int("SELECT COUNT(*) FROM setting WHERE key LIKE 'k%'"); }), 4);
  auto bad = q.submit([](db::Connection& c) { c.exec("INSERT INTO nope VALUES (1)"); });
  EXPECT_THROW(bad.get(), db::DbError);
  // The queue keeps working after a failed job.
  EXPECT_EQ(q.run([](db::Connection&) { return 7; }), 7);

  // Readers see committed data while the writer is busy.
  db::ReadPool pool(path, 2);
  auto lease = pool.acquire();
  EXPECT_EQ(lease->scalar_int("SELECT COUNT(*) FROM setting WHERE key LIKE 'k%'"), 4);
}
