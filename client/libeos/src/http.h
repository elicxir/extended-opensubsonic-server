// HTTP over libcurl. Easy handles are pooled: each keeps its connection open, so
// requests reuse connections and several threads can send at once.
#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "eos/client/api.h"

namespace eos::client {

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string content_type;
};

std::string url_encode(const std::string& s);
std::string encode_query(const Query& q);

class Http {
 public:
  explicit Http(Options options);
  ~Http();
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  const Options& options() const { return options_; }

  // base_url + path [+ "?" + query]
  std::string url(const std::string& path, const Query& query = {}) const;

  // Throws Error(network) when no response arrives; any HTTP status is returned.
  // `body` is sent when `body_type` is not empty. The API key goes in X-Api-Key.
  HttpResponse send(const std::string& method, const std::string& path, const Query& query = {},
                    const std::string& body = {}, const std::string& body_type = {});

 private:
  void* acquire();
  void release(void* handle);

  Options options_;
  std::mutex mutex_;
  std::vector<void*> idle_;
};

}  // namespace eos::client
