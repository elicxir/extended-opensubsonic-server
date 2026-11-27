#include <gtest/gtest.h>

#include <cstdlib>

#include "core/config.h"

using namespace eos;

TEST(Config, DefaultsAndLibraries) {
  auto cfg = parse_config(R"(
[server]
data_dir = "/data"

[[library]]
name = "Music"
path = "/music"

[scanner]
interval = "15m"
extensions = ["FLAC"]
)");
  EXPECT_EQ(cfg.data_dir, "/data");
  EXPECT_EQ(cfg.db_path(), "/data/eos.db");
  EXPECT_EQ(cfg.secret_key_path(), "/data/secret.key");
  ASSERT_EQ(cfg.libraries.size(), 1u);
  EXPECT_EQ(cfg.libraries[0].path, "/music");
  EXPECT_EQ(cfg.scan_interval_seconds, 900);
  EXPECT_EQ(cfg.extensions, (std::vector<std::string>{"flac"}));
  EXPECT_TRUE(cfg.allow_token);
  EXPECT_FALSE(cfg.allow_password);
  EXPECT_EQ(cfg.listen, "0.0.0.0:4540");
  EXPECT_TRUE(cfg.tags.at("collection").album_level);
  EXPECT_FALSE(cfg.tags.at("collection_work").album_level);
}

TEST(Config, CustomTagsOverrideAndAdd) {
  auto cfg = parse_config(R"(
[tags.collection_work]
aliases = ["collection_work", "soundtrack_for"]

[tags.mood]
split = [","]
)");
  EXPECT_EQ(cfg.tags.at("collection_work").aliases,
            (std::vector<std::string>{"COLLECTION_WORK", "SOUNDTRACK_FOR"}));
  EXPECT_EQ(cfg.tags.at("mood").aliases, (std::vector<std::string>{"MOOD"}));
}

TEST(Config, BadValuesThrow) {
  EXPECT_THROW(parse_config("[scanner]\ninterval = \"soon\""), ConfigError);
  EXPECT_THROW(parse_config("this is = = not toml"), ConfigError);
}

TEST(Config, EnvOverrides) {
  Config cfg;
  setenv("EOS_LIBRARY_PATH", "/srv/music", 1);
  setenv("EOS_SERVER_DATA_DIR", "/tmp/eosdata", 1);
  apply_env_overrides(cfg);
  unsetenv("EOS_LIBRARY_PATH");
  unsetenv("EOS_SERVER_DATA_DIR");
  ASSERT_EQ(cfg.libraries.size(), 1u);
  EXPECT_EQ(cfg.libraries[0].path, "/srv/music");
  EXPECT_EQ(cfg.data_dir, "/tmp/eosdata");
}
