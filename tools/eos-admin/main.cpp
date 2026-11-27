// eos-admin: administration CLI for eos. Works on the same config and database as eosd
// and is safe to run while eosd is running (SQLite WAL + busy timeout).

#include <sqlite3.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "auth/users.h"
#include "core/config.h"
#include "core/crypto.h"
#include "core/log.h"
#include "db/migrate.h"
#include "db/write_queue.h"
#include "scanner/scanner.h"
#include "scanner/tag_report.h"

namespace {
using namespace eos;

constexpr const char* kUsage = R"(usage: eos-admin [--config PATH] <command> [args]

commands:
  user add <name> [--admin]       create a user; the password is read from stdin
  user passwd <name>              change a password (read from stdin)
  user list
  apikey create <user> <device>   issue an API key for one device (printed once)
  apikey list <user>
  apikey revoke <key-id>
  scan [--full]                   scan the libraries now
  tag-report [--json]             how the library is tagged (run a scan first)
  backup <file>                   copy the database while it is in use
)";

std::string read_line_no_echo(const std::string& prompt) {
  std::string line;
  if (isatty(STDIN_FILENO)) {
    std::cerr << prompt << std::flush;
    termios old_t{};
    tcgetattr(STDIN_FILENO, &old_t);
    termios t = old_t;
    t.c_lflag &= static_cast<tcflag_t>(~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    std::getline(std::cin, line);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
    std::cerr << "\n";
  } else {
    std::getline(std::cin, line);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

std::string read_password() {
  auto p1 = read_line_no_echo("Password: ");
  if (p1.empty()) throw std::runtime_error("empty password");
  if (isatty(STDIN_FILENO)) {
    auto p2 = read_line_no_echo("Repeat password: ");
    if (p1 != p2) throw std::runtime_error("passwords do not match");
  }
  return p1;
}

std::string fmt_time(std::optional<int64_t> ms) {
  if (!ms) return "-";
  auto tp = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(*ms));
  return std::format("{:%Y-%m-%d %H:%M}", std::chrono::floor<std::chrono::minutes>(tp));
}

auth::User require_user(db::Connection& c, const std::string& name) {
  auto u = auth::find_user(c, name);
  if (!u) throw std::runtime_error(std::format("no such user: {}", name));
  return *u;
}

int run(const Config& cfg, const std::vector<std::string>& args) {
  auto need = [&](size_t n) {
    if (args.size() < n) throw std::invalid_argument("missing arguments");
  };
  need(1);
  const auto& cmd = args[0];

  std::filesystem::create_directories(cfg.data_dir);
  {
    db::Connection c(cfg.db_path());
    db::migrate(c);
  }

  if (cmd == "user") {
    need(2);
    db::Connection c(cfg.db_path());
    if (args[1] == "add") {
      need(3);
      bool admin = args.size() > 3 && args[3] == "--admin";
      auto key = crypto::load_or_create_secret_key(cfg.secret_key_path());
      auto password = read_password();
      auto u = auth::create_user(c, args[2], password, admin, key);
      std::cout << std::format("created user {} (id {}{})\n", u.username, u.id, admin ? ", admin" : "");
      return 0;
    }
    if (args[1] == "passwd") {
      need(3);
      auto u = require_user(c, args[2]);
      auto key = crypto::load_or_create_secret_key(cfg.secret_key_path());
      auth::set_password(c, u.id, read_password(), key);
      std::cout << "password changed\n";
      return 0;
    }
    if (args[1] == "list") {
      for (const auto& u : auth::list_users(c))
        std::cout << std::format("{:<20} {:<6} created {}\n", u.username, u.is_admin ? "admin" : "user",
                                 fmt_time(u.created_at));
      return 0;
    }
  }

  if (cmd == "apikey") {
    need(2);
    db::Connection c(cfg.db_path());
    if (args[1] == "create") {
      need(4);
      auto u = require_user(c, args[2]);
      auto k = auth::create_api_key(c, u.id, args[3]);
      std::cerr << std::format("API key {} for {} on '{}'. It is shown only once:\n", k.id, u.username,
                               args[3]);
      std::cout << k.key << "\n";
      return 0;
    }
    if (args[1] == "list") {
      need(3);
      auto u = require_user(c, args[2]);
      for (const auto& k : auth::list_api_keys(c, u.id))
        std::cout << std::format("{}  {:<20} created {}  last used {}  {}\n", k.id, k.device_name,
                                 fmt_time(k.created_at), fmt_time(k.last_used_at),
                                 k.revoked_at ? "revoked" : "active");
      return 0;
    }
    if (args[1] == "revoke") {
      need(3);
      if (!auth::revoke_api_key(c, args[2])) throw std::runtime_error("no active key with that id");
      std::cout << "revoked\n";
      return 0;
    }
  }

  if (cmd == "scan") {
    bool full = args.size() > 1 && args[1] == "--full";
    db::WriteQueue writer(cfg.db_path());
    scanner::Scanner s(cfg, writer, cfg.db_path());
    auto stats = s.scan(full);
    std::cout << std::format(
        "{} files: {} added, {} updated, {} moved, {} restored, {} missing, {} unchanged, {} errors\n",
        stats.seen, stats.added, stats.updated, stats.moved, stats.restored, stats.missing,
        stats.unchanged, stats.errors.size());
    for (const auto& e : stats.errors) std::cout << "  error: " << e << "\n";
    return stats.errors.empty() ? 0 : 1;
  }

  if (cmd == "tag-report") {
    db::Connection c(cfg.db_path(), db::Connection::Mode::read_only);
    auto report = scanner::build_tag_report(c);
    bool json = args.size() > 1 && args[1] == "--json";
    std::cout << (json ? scanner::tag_report_json(report) + "\n" : scanner::format_tag_report(report));
    return 0;
  }

  if (cmd == "backup") {
    need(2);
    db::Connection src(cfg.db_path(), db::Connection::Mode::read_only);
    db::Connection dst(args[1]);
    auto* b = sqlite3_backup_init(dst.raw(), "main", src.raw(), "main");
    if (!b) throw std::runtime_error(sqlite3_errmsg(dst.raw()));
    int rc;
    while ((rc = sqlite3_backup_step(b, 256)) == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
      sqlite3_sleep(50);
    sqlite3_backup_finish(b);
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errstr(rc));
    std::cout << "backup written to " << args[1] << "\n";
    return 0;
  }

  throw std::invalid_argument("unknown command");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path = eos::default_config_path();
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "-h" || a == "--help") {
      std::cout << kUsage;
      return 0;
    } else {
      args.push_back(a);
    }
  }
  if (args.empty()) {
    std::cerr << kUsage;
    return 2;
  }
  try {
    auto cfg = eos::load_config(config_path);
    eos::log::set_level(cfg.log_level);
    return run(cfg, args);
  } catch (const std::invalid_argument& e) {
    std::cerr << "error: " << e.what() << "\n\n" << kUsage;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
