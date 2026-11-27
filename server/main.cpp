// eosd: the eos music server.
//
// Loads the config, prepares the data directory and database, keeps the library index
// up to date (scan at start-up, then every scanner.interval) and serves the
// OpenSubsonic API on server.listen.

#include <signal.h>

#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#include "core/config.h"
#include "core/crypto.h"
#include "core/log.h"
#include "api/ext/router.h"
#include "api/subsonic/router.h"
#include "db/migrate.h"
#include "http/server.h"
#include "db/write_queue.h"
#include "scanner/scanner.h"

namespace {

void usage() {
  std::cout << "usage: eosd [--config PATH] [--scan-once]\n"
               "  --config PATH  config file (default: $EOS_CONFIG or /etc/eos/eos.toml)\n"
               "  --scan-once    scan the libraries once and exit\n"
               "  --version      print the version\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace eos;
  std::filesystem::path config_path = default_config_path();
  bool scan_once = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--scan-once") {
      scan_once = true;
    } else if (arg == "--version") {
      std::cout << "eosd " << EOS_VERSION << "\n";
      return 0;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }

  // Block termination signals before any thread starts; a dedicated thread waits for them.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  try {
    auto cfg = load_config(config_path);
    if (!log::set_level(cfg.log_level)) log::warn("unknown log level '{}', using info", cfg.log_level);
    log::info("eosd {} starting, config {}", EOS_VERSION, config_path.string());

    std::filesystem::create_directories(cfg.data_dir);
    crypto::load_or_create_secret_key(cfg.secret_key_path());
    {
      db::Connection conn(cfg.db_path());
      auto version = db::migrate(conn);
      log::info("database {} at schema version {}", cfg.db_path().string(), version);
    }

    db::WriteQueue writer(cfg.db_path());
    scanner::Scanner scanner(cfg, writer, cfg.db_path());

    if (scan_once) {
      auto stats = scanner.scan(false);
      return stats.errors.empty() ? 0 : 1;
    }

    auto secret = crypto::load_or_create_secret_key(cfg.secret_key_path());
    db::ReadPool reads(cfg.db_path(), 4);
    api::Services services(cfg, reads, writer, scanner, secret);
    api::Router router(services);
    api::ext::ExtRouter ext_router(services);

    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;

    // Periodic scans run beside the HTTP server.
    std::thread scan_thread([&] {
      while (true) {
        try {
          scanner.scan(false);
        } catch (const std::exception& e) {
          log::error("scan failed: {}", e.what());
        }
        std::unique_lock lock(mutex);
        if (cfg.scan_interval_seconds <= 0) {
          cv.wait(lock, [&] { return stop; });
        } else {
          cv.wait_for(lock, std::chrono::seconds(cfg.scan_interval_seconds), [&] { return stop; });
        }
        if (stop) return;
      }
    });

    std::thread signal_thread([&] {
      int sig = 0;
      sigwait(&signals, &sig);
      log::info("received {}, shutting down", strsignal(sig));
      {
        std::lock_guard lock(mutex);
        stop = true;
      }
      cv.notify_all();
      http::stop();
    });

    auto scratch = cfg.cache_dir() / "http";
    std::filesystem::create_directories(scratch);
    std::vector<http::Route> routes = {
        {"/rest/.*", [&](const api::Request& r) { return router.handle(r); }},
        {"/ext/.*", [&](const api::Request& r) { return ext_router.handle(r); }},
    };
    http::run(routes, cfg.listen, cfg.http_threads, scratch.string());  // blocks until http::stop()
    signal_thread.join();
    scan_thread.join();
    writer.stop();
    log::info("stopped");
    return 0;
  } catch (const std::exception& e) {
    log::error("fatal: {}", e.what());
    return 1;
  }
}
