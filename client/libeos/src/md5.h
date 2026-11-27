// MD5, for the Subsonic token (t = md5(password + salt)). Not for anything else:
// MD5 is only here because that protocol asks for it.
#pragma once

#include <string>

namespace eos::client {

// The digest as 32 lowercase hex digits.
std::string md5_hex(const std::string& data);

}  // namespace eos::client
