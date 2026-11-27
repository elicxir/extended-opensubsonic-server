#pragma once

#include <vector>

#include "db/database.h"

namespace eos::db {

struct Migration {
  int version;
  const char* name;
  const char* sql;
};

// Compiled in from server/db/migrations/*.sql (see cmake/embed_migrations.cmake).
const std::vector<Migration>& embedded_migrations();

// Applies every migration newer than the recorded schema version, each in its own
// transaction. Returns the schema version after migrating.
int migrate(Connection& conn);
int migrate(Connection& conn, const std::vector<Migration>& migrations);

}  // namespace eos::db
