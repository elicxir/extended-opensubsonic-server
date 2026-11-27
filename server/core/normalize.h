#pragma once

#include <string>
#include <string_view>

namespace eos {

// The one normalisation used for search, PIDs and comparing names:
//   1. NFKC + case folding (utf8proc NFKC_Casefold)
//   2. katakana -> hiragana (U+30A1..U+30F6 -> U+3041..U+3096; the prolonged sound mark stays)
//   3. drop whitespace, punctuation (P*), symbols (S*) and control characters
// Invalid UTF-8 is treated as Latin-1 bytes so the function never fails.
std::string normalize(std::string_view input);

}  // namespace eos
