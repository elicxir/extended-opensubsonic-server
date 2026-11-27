#include <gtest/gtest.h>

#include "core/normalize.h"

using eos::normalize;

TEST(Normalize, DesignExample) {
  // The example from the design doc (検索 / 正規化).
  EXPECT_EQ(normalize("「Final Fantasy」〜ピアノ版〜"), "finalfantasyぴあの版");
}

TEST(Normalize, WidthCaseAndKana) {
  EXPECT_EQ(normalize("ＡＢＣ　abc"), "abcabc");
  EXPECT_EQ(normalize("ｶﾀｶﾅ"), "かたかな");
  EXPECT_EQ(normalize("ヴァイオリン"), "ゔぁいおりん");
  EXPECT_EQ(normalize("ラーメン"), "らーめん");  // the prolonged sound mark stays
}

TEST(Normalize, DropsPunctuationAndSymbols) {
  EXPECT_EQ(normalize("Off-Vocal (TV Size) ~ver.2~"), "offvocaltvsizever2");
  EXPECT_EQ(normalize("A・B"), "ab");
  EXPECT_EQ(normalize(""), "");
}

TEST(Normalize, InvalidUtf8DoesNotThrow) {
  std::string bad = "caf\xe9";  // Latin-1 é
  EXPECT_EQ(normalize(bad), "café");
}
