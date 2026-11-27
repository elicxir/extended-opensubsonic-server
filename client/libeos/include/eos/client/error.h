#pragma once

#include <stdexcept>
#include <string>

namespace eos::client {

// Every failed call throws Error.
class Error : public std::runtime_error {
 public:
  enum class Kind {
    network,   // could not reach the server (DNS, connect, TLS, timeout)
    http,      // an HTTP status that carries no API error body
    subsonic,  // /rest answered status "failed"; see subsonic_code()
    ext,       // /ext answered an error body; see http_status() and ext_code()
    parse,     // the response was not what the API promises
  };

  Error(Kind kind, const std::string& message, long http_status = 0, int subsonic_code = -1,
        std::string ext_code = {})
      : std::runtime_error(message),
        kind_(kind),
        http_status_(http_status),
        subsonic_code_(subsonic_code),
        ext_code_(std::move(ext_code)) {}

  Kind kind() const { return kind_; }
  long http_status() const { return http_status_; }           // 0 when there was no response
  int subsonic_code() const { return subsonic_code_; }        // -1 unless kind() == subsonic
  const std::string& ext_code() const { return ext_code_; }   // e.g. "not_found", "tag_managed"

  // The item does not exist (Subsonic 70, /ext 404).
  bool not_found() const {
    return (kind_ == Kind::subsonic && subsonic_code_ == 70) || (kind_ == Kind::ext && http_status_ == 404);
  }
  // The API key (or the login) was not accepted.
  bool unauthorized() const {
    return (kind_ == Kind::subsonic && subsonic_code_ >= 40 && subsonic_code_ <= 44) || http_status_ == 401;
  }
  // The user may not do this (admin only, or someone else's playlist).
  bool forbidden() const { return (kind_ == Kind::subsonic && subsonic_code_ == 50) || http_status_ == 403; }
  // Worth retrying later: the server could not be reached.
  bool offline() const { return kind_ == Kind::network; }

 private:
  Kind kind_;
  long http_status_;
  int subsonic_code_;
  std::string ext_code_;
};

}  // namespace eos::client
