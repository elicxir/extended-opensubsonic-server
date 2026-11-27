#include <gtest/gtest.h>

#include "core/strings.h"

using namespace eos;

TEST(Strings, SplitAnyDropsEmptyPieces) {
  EXPECT_EQ(split_any("A; B / C", {"; ", " / "}), (std::vector<std::string>{"A", "B", "C"}));
  EXPECT_EQ(split_any(" A ;; ", {";"}), (std::vector<std::string>{"A"}));
  EXPECT_EQ(split_any("AC/DC", {}), (std::vector<std::string>{"AC/DC"}));
}

TEST(Strings, ParseLeadingInt) {
  EXPECT_EQ(parse_leading_int("3/12"), 3);
  EXPECT_EQ(parse_leading_int(" 07 "), 7);
  EXPECT_EQ(parse_leading_int("x"), std::nullopt);
}

TEST(Strings, ParseDuration) {
  EXPECT_EQ(parse_duration_seconds("60m"), 3600);
  EXPECT_EQ(parse_duration_seconds("1h"), 3600);
  EXPECT_EQ(parse_duration_seconds("45"), 45);
  EXPECT_EQ(parse_duration_seconds("2d"), 172800);
  EXPECT_EQ(parse_duration_seconds("10x"), std::nullopt);
}
