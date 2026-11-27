#include <gtest/gtest.h>

#include <map>

#include "core/ids.h"
#include "scanner/pid.h"

using namespace eos;

TEST(Ids, Base62FixedWidth) {
  std::vector<uint8_t> zero(16, 0), max(16, 0xff);
  EXPECT_EQ(ids::base62(zero), std::string(22, '0'));
  EXPECT_EQ(ids::base62(max).size(), 22u);
  EXPECT_EQ(ids::base62(std::vector<uint8_t>(32, 0xff)).size(), 43u);
  EXPECT_EQ(ids::base62(std::vector<uint8_t>{0, 61}).back(), 'z');
}

TEST(Ids, RandomAndHash) {
  EXPECT_NE(ids::random_id(), ids::random_id());
  EXPECT_EQ(ids::hash_id("a"), ids::hash_id("a"));
  EXPECT_NE(ids::hash_id("a"), ids::hash_id("b"));
  EXPECT_EQ(ids::hash_id("a").size(), 22u);
}

TEST(Pid, FirstCompleteAlternativeWins) {
  auto spec = scanner::PidSpec::parse("musicbrainz_trackid|albumid,discnumber,tracknumber,title");
  std::map<std::string, std::string> v = {
      {"albumid", "ALB"}, {"discnumber", "1"}, {"tracknumber", "2"}, {"title", "Song"}};
  auto lookup = [&](std::string_view f) {
    auto it = v.find(std::string(f));
    return it == v.end() ? std::string() : it->second;
  };
  auto without_mbid = scanner::compute_pid(spec, lookup);
  v["musicbrainz_trackid"] = "abc-123";
  auto with_mbid = scanner::compute_pid(spec, lookup);
  EXPECT_NE(without_mbid, with_mbid);
  // Only the MBID matters once it is present.
  v["title"] = "Other";
  EXPECT_EQ(scanner::compute_pid(spec, lookup), with_mbid);
}

TEST(Pid, ValuesAreNormalised) {
  auto spec = scanner::PidSpec::parse("title");
  auto a = scanner::compute_pid(spec, [](std::string_view) { return std::string("Song (TV Size)"); });
  auto b = scanner::compute_pid(spec, [](std::string_view) { return std::string("song tv size"); });
  EXPECT_EQ(a, b);
}
