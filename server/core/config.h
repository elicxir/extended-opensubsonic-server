#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace eos {

struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct LibraryConfig {
  std::string name;
  std::filesystem::path path;
};

// A custom tag, declared like Navidrome's `Tags` option.
struct CustomTagConfig {
  std::vector<std::string> aliases;  // upper-cased tag names that map to this tag
  std::vector<std::string> split;    // separators inside one value
  bool album_level = false;          // level = "album": values are merged across the album
  size_t max_length = 1024;
};

struct Config {
  // [server]
  std::string listen = "0.0.0.0:4540";
  std::filesystem::path data_dir = "/var/lib/eos";
  std::filesystem::path secret_key;  // empty -> <data_dir>/secret.key
  std::string log_level = "info";

  std::vector<LibraryConfig> libraries;

  // [scanner]
  long long scan_interval_seconds = 3600;  // 0 disables periodic scans
  std::vector<std::string> extensions = {"flac", "mp3", "m4a", "ogg", "opus"};
  int scan_workers = 2;
  int purge_missing_after_days = 30;
  std::vector<std::string> artist_separators = {"; ", " / "};

  // [pid]
  std::string pid_track = "musicbrainz_trackid|albumid,discnumber,tracknumber,title";
  std::string pid_album = "musicbrainz_albumid|albumartistid,album,albumversion,releasedate";

  // [tags.<name>]
  std::map<std::string, CustomTagConfig> tags;

  // [auth]
  bool allow_token = true;
  bool allow_password = false;
  std::vector<std::string> rate_limit_exempt = {"127.0.0.0/8", "::1",          "10.0.0.0/8",
                                                "172.16.0.0/12", "192.168.0.0/16", "fc00::/7",
                                                "fe80::/10"};

  std::filesystem::path db_path() const { return data_dir / "eos.db"; }
  std::filesystem::path secret_key_path() const {
    return secret_key.empty() ? data_dir / "secret.key" : secret_key;
  }
};

// Built-in custom tags (collection_work, collection, collection_album).
std::map<std::string, CustomTagConfig> default_custom_tags();

// Parses TOML text. Unknown keys are ignored with a warning.
Config parse_config(std::string_view toml_text);

// Reads the file, then applies environment overrides:
//   EOS_SERVER_LISTEN, EOS_SERVER_DATA_DIR, EOS_SERVER_LOG_LEVEL,
//   EOS_LIBRARY_PATH (replaces the library list with one library "Music").
Config load_config(const std::filesystem::path& path);
void apply_env_overrides(Config& cfg);

// Where to find the config: --config wins, then $EOS_CONFIG, then /etc/eos/eos.toml.
std::filesystem::path default_config_path();

}  // namespace eos
