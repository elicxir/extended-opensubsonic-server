#include "db/migrate.h"

#include <format>

#include "core/log.h"

namespace eos::db {

int migrate(Connection& conn) { return migrate(conn, embedded_migrations()); }

int migrate(Connection& conn, const std::vector<Migration>& migrations) {
  conn.exec("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)");
  int current = static_cast<int>(conn.scalar_int("SELECT MAX(version) FROM schema_version").value_or(0));
  int latest = current;
  for (const auto& m : migrations) {
    if (m.version <= current) continue;
    log::info("db: applying migration {}", m.name);
    Transaction tx(conn);
    conn.exec(m.sql);
    conn.run("INSERT INTO schema_version (version) VALUES (?)", m.version);
    tx.commit();
    latest = m.version;
  }
  if (!migrations.empty() && current > migrations.back().version) {
    throw DbError(0, std::format("database schema version {} is newer than this build ({})", current,
                                 migrations.back().version));
  }
  return latest;
}

}  // namespace eos::db
