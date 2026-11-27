#include "core/config.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "core/log.h"
#include "core/strings.h"

namespace eos {
namespace {

std::vector<std::string> string_array(const toml::node* node, std::string_view key) {
  std::vector<std::string> out;
  if (!node) return out;
  auto arr = node->as_array();
  if (!arr) throw ConfigError(std::format("{} must be an array of strings", key));
  for (const auto& el : *arr) {
    auto s = el.value<std::string>();
    if (!s) throw ConfigError(std::format("{} must be an array of strings", key));
    out.push_back(*s);
  }
  return out;
}

void warn_unknown(const toml::table& table, std::string_view section,
                  std::initializer_list<std::string_view> known) {
  for (const auto& [k, _] : table) {
    bool ok = false;
    for (auto name : known) ok = ok || k.str() == name;
    if (!ok) log::warn("config: unknown key {}{}{}", section, section.empty() ? "" : ".", k.str());
  }
}

}  // namespace

std::map<std::string, CustomTagConfig> default_custom_tags() {
  std::map<std::string, CustomTagConfig> tags;
  tags["collection_work"] = {{"COLLECTION_WORK"}, {"; "}, false, 1024};
  tags["collection"] = {{"COLLECTION"}, {"; "}, true, 1024};
  tags["collection_album"] = {{"COLLECTION_ALBUM"}, {"; "}, false, 1024};
  return tags;
}

Config parse_config(std::string_view text) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& e) {
    std::ostringstream os;
    os << e.description() << " (line " << e.source().begin.line << ")";
    throw ConfigError("config: " + os.str());
  }

  Config cfg;
  cfg.tags = default_custom_tags();
  warn_unknown(root, "", {"server", "http", "library", "scanner", "pid", "tags", "suggest",
                          "transcode", "auth", "artwork", "estimate", "subsonic"});

  if (auto server = root["server"].as_table()) {
    warn_unknown(*server, "server", {"listen", "data_dir", "secret_key", "log_level"});
    cfg.listen = (*server)["listen"].value_or(cfg.listen);
    cfg.data_dir = (*server)["data_dir"].value_or(cfg.data_dir.string());
    cfg.secret_key = (*server)["secret_key"].value_or(std::string());
    cfg.log_level = (*server)["log_level"].value_or(cfg.log_level);
  }

  if (auto http = root["http"].as_table()) {
    warn_unknown(*http, "http", {"threads", "trusted_proxies"});
    cfg.http_threads = static_cast<int>((*http)["threads"].value_or(int64_t{cfg.http_threads}));
    if ((*http)["trusted_proxies"])
      cfg.trusted_proxies = string_array((*http)["trusted_proxies"].node(), "http.trusted_proxies");
  }

  if (auto art = root["artwork"].as_table()) {
    if ((*art)["album_files"])
      cfg.artwork_album_files = string_array((*art)["album_files"].node(), "artwork.album_files");
    cfg.artwork_cache_max_mb =
        static_cast<int>((*art)["cache_max_mb"].value_or(int64_t{cfg.artwork_cache_max_mb}));
  }

  if (auto sub = root["subsonic"].as_table()) {
    cfg.ignored_articles = (*sub)["ignored_articles"].value_or(cfg.ignored_articles);
    auto va = (*sub)["virtual_albums"].value_or(std::string("hide"));
    if (va != "hide" && va != "show") throw ConfigError("subsonic.virtual_albums must be \"hide\" or \"show\"");
    cfg.show_virtual_albums = va == "show";
  }

  if (auto tc = root["transcode"].as_table()) {
    warn_unknown(*tc, "transcode", {"ffmpeg", "default_format", "max_concurrent"});
    cfg.transcode_ffmpeg = (*tc)["ffmpeg"].value_or(cfg.transcode_ffmpeg);
    cfg.transcode_default_format = (*tc)["default_format"].value_or(cfg.transcode_default_format);
    cfg.transcode_max_concurrent =
        static_cast<int>((*tc)["max_concurrent"].value_or(int64_t{cfg.transcode_max_concurrent}));
  }

  if (auto est = root["estimate"].as_table()) {
    if (auto v = (*est)["auto_apply_min_confidence"].value<double>()) cfg.estimate_auto_apply_min_confidence = *v;
    else if (auto vi = (*est)["auto_apply_min_confidence"].value<int64_t>())
      cfg.estimate_auto_apply_min_confidence = static_cast<double>(*vi);
  }

  if (auto sg = root["suggest"].as_table()) {
    if ((*sg)["derivative_patterns"])
      cfg.derivative_patterns = string_array((*sg)["derivative_patterns"].node(), "suggest.derivative_patterns");
    if ((*sg)["medley_separators"])
      cfg.medley_separators = string_array((*sg)["medley_separators"].node(), "suggest.medley_separators");
  }

  if (auto libs = root["library"].as_array()) {
    for (const auto& el : *libs) {
      auto t = el.as_table();
      if (!t) throw ConfigError("[[library]] entries must be tables");
      LibraryConfig lib;
      lib.name = (*t)["name"].value_or(std::string("Music"));
      auto path = (*t)["path"].value<std::string>();
      if (!path) throw ConfigError("[[library]] needs a path");
      lib.path = *path;
      cfg.libraries.push_back(std::move(lib));
    }
  }

  if (auto sc = root["scanner"].as_table()) {
    warn_unknown(*sc, "scanner", {"interval", "extensions", "workers", "purge_missing_after_days",
                                  "artist_separators"});
    if (auto iv = (*sc)["interval"].value<std::string>()) {
      auto secs = parse_duration_seconds(*iv);
      if (!secs) throw ConfigError(std::format("scanner.interval: cannot parse '{}'", *iv));
      cfg.scan_interval_seconds = *secs;
    } else if (auto ivn = (*sc)["interval"].value<int64_t>()) {
      cfg.scan_interval_seconds = *ivn;
    }
    if ((*sc)["extensions"]) cfg.extensions = string_array((*sc)["extensions"].node(), "scanner.extensions");
    cfg.scan_workers = static_cast<int>((*sc)["workers"].value_or(int64_t{cfg.scan_workers}));
    cfg.purge_missing_after_days = static_cast<int>(
        (*sc)["purge_missing_after_days"].value_or(int64_t{cfg.purge_missing_after_days}));
    if ((*sc)["artist_separators"])
      cfg.artist_separators = string_array((*sc)["artist_separators"].node(), "scanner.artist_separators");
  }
  for (auto& e : cfg.extensions) e = to_lower_ascii(e);
  if (cfg.scan_workers < 1) cfg.scan_workers = 1;

  if (auto pid = root["pid"].as_table()) {
    cfg.pid_track = (*pid)["track"].value_or(cfg.pid_track);
    cfg.pid_album = (*pid)["album"].value_or(cfg.pid_album);
  }

  if (auto tags = root["tags"].as_table()) {
    for (const auto& [name, node] : *tags) {
      auto t = node.as_table();
      if (!t) throw ConfigError(std::format("tags.{} must be a table", name.str()));
      CustomTagConfig tag;
      auto existing = cfg.tags.find(std::string(name.str()));
      if (existing != cfg.tags.end()) tag = existing->second;
      if ((*t)["aliases"]) {
        tag.aliases.clear();
        for (auto& a : string_array((*t)["aliases"].node(), "aliases")) tag.aliases.push_back(to_upper_ascii(a));
      }
      if ((*t)["split"]) tag.split = string_array((*t)["split"].node(), "split");
      if (auto level = (*t)["level"].value<std::string>()) tag.album_level = (*level == "album");
      tag.max_length = static_cast<size_t>((*t)["max_length"].value_or(int64_t(tag.max_length)));
      if (tag.aliases.empty()) tag.aliases.push_back(to_upper_ascii(name.str()));
      cfg.tags[std::string(name.str())] = std::move(tag);
    }
  }

  if (auto auth = root["auth"].as_table()) {
    cfg.allow_token = (*auth)["allow_token"].value_or(cfg.allow_token);
    cfg.allow_password = (*auth)["allow_password"].value_or(cfg.allow_password);
    if ((*auth)["rate_limit_exempt"])
      cfg.rate_limit_exempt = string_array((*auth)["rate_limit_exempt"].node(), "auth.rate_limit_exempt");
  }

  return cfg;
}

void apply_env_overrides(Config& cfg) {
  if (auto v = std::getenv("EOS_SERVER_LISTEN")) cfg.listen = v;
  if (auto v = std::getenv("EOS_SERVER_DATA_DIR")) cfg.data_dir = v;
  if (auto v = std::getenv("EOS_SERVER_LOG_LEVEL")) cfg.log_level = v;
  if (auto v = std::getenv("EOS_LIBRARY_PATH")) cfg.libraries = {LibraryConfig{"Music", v}};
}

Config load_config(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) throw ConfigError(std::format("cannot read config file {}", path.string()));
  std::stringstream ss;
  ss << in.rdbuf();
  auto cfg = parse_config(ss.str());
  apply_env_overrides(cfg);
  if (cfg.libraries.empty()) throw ConfigError("config: no [[library]] configured");
  return cfg;
}

std::filesystem::path default_config_path() {
  if (auto v = std::getenv("EOS_CONFIG"); v && *v) return v;
  return "/etc/eos/eos.toml";
}

}  // namespace eos
