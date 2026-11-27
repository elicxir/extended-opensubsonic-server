#include "http.h"

#include <curl/curl.h>

namespace eos::client {
namespace {

void global_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t on_body(char* data, size_t size, size_t n, void* out) {
  static_cast<std::string*>(out)->append(data, size * n);
  return size * n;
}

// Idle handles kept per Http. More may exist while requests overlap.
constexpr size_t kMaxIdle = 4;

}  // namespace

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string encode_query(const Query& q) {
  std::string out;
  for (const auto& [k, v] : q) {
    if (!out.empty()) out += '&';
    out += url_encode(k);
    out += '=';
    out += url_encode(v);
  }
  return out;
}

Http::Http(Options options) : options_(std::move(options)) {
  global_init();
  while (!options_.base_url.empty() && options_.base_url.back() == '/') options_.base_url.pop_back();
}

Http::~Http() {
  for (void* h : idle_) curl_easy_cleanup(static_cast<CURL*>(h));
}

std::string Http::url(const std::string& path, const Query& query) const {
  std::string out = options_.base_url + path;
  if (!query.empty()) out += "?" + encode_query(query);
  return out;
}

void* Http::acquire() {
  {
    std::lock_guard lock(mutex_);
    if (!idle_.empty()) {
      void* h = idle_.back();
      idle_.pop_back();
      return h;
    }
  }
  CURL* h = curl_easy_init();
  if (!h) throw Error(Error::Kind::network, "curl_easy_init failed");
  return h;
}

void Http::release(void* handle) {
  {
    std::lock_guard lock(mutex_);
    if (idle_.size() < kMaxIdle) {
      idle_.push_back(handle);
      return;
    }
  }
  curl_easy_cleanup(static_cast<CURL*>(handle));
}

HttpResponse Http::send(const std::string& method, const std::string& path, const Query& query,
                        const std::string& body, const std::string& body_type) {
  if (options_.base_url.empty()) throw Error(Error::Kind::network, "No server address is set");

  struct Lease {
    Http& http;
    CURL* h;
    ~Lease() { http.release(h); }
  } lease{*this, static_cast<CURL*>(acquire())};
  CURL* h = lease.h;
  curl_easy_reset(h);  // keeps the open connection

  HttpResponse res;
  char errbuf[CURL_ERROR_SIZE] = {0};
  const std::string target = url(path, query);
  curl_easy_setopt(h, CURLOPT_URL, target.c_str());
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, options_.connect_timeout_ms);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, options_.timeout_ms);
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &res.body);
  const std::string agent = options_.client_name + " (libeos/" LIBEOS_VERSION ")";
  curl_easy_setopt(h, CURLOPT_USERAGENT, agent.c_str());

  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json, */*");
  if (!options_.api_key.empty()) headers = curl_slist_append(headers, ("X-Api-Key: " + options_.api_key).c_str());
  if (!body_type.empty()) {
    headers = curl_slist_append(headers, ("Content-Type: " + body_type).c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  }
  if (method != "GET" && !(method == "POST" && !body_type.empty()))
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

  const CURLcode rc = curl_easy_perform(h);
  curl_slist_free_all(headers);
  if (rc != CURLE_OK) {
    const std::string detail = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    throw Error(Error::Kind::network, "Cannot reach " + options_.base_url + ": " + detail);
  }
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
  char* type = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &type) == CURLE_OK && type) res.content_type = type;
  return res;
}

}  // namespace eos::client
