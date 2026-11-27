#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eos::media {
class StreamSource;
}

namespace eos::api {

// A transport-independent HTTP request. The Drogon adapter fills it in; tests build
// it directly.
struct Request {
  std::string method = "GET";
  std::string path;                                 // e.g. "/rest/ping.view"
  std::vector<std::pair<std::string, std::string>> params;  // query + form, in order
  std::map<std::string, std::string> headers;       // lower-case names
  std::string peer_ip;                               // TCP peer
  std::string body;                                  // raw body (JSON for /ext)

  std::optional<std::string> param(std::string_view name) const;
  std::vector<std::string> params_all(std::string_view name) const;
  bool has(std::string_view name) const { return param(name).has_value(); }
  std::optional<std::string> header(std::string_view lower_name) const;
};

struct Response {
  int status = 200;
  std::string content_type = "text/plain";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  // A file response: the adapter sends bytes [offset, offset + length) of `file`.
  std::optional<std::filesystem::path> file;
  int64_t offset = 0;
  int64_t length = 0;

  // A generated body (transcoding), sent chunked as it is produced.
  std::shared_ptr<media::StreamSource> stream;
};

std::string url_decode(std::string_view s, bool plus_is_space = true);
// Parses "a=1&b=2&a=3" into ordered pairs (values url-decoded).
void parse_query(std::string_view query, std::vector<std::pair<std::string, std::string>>& out);

// A single byte range resolved against a file size.
struct ByteRange {
  int64_t start = 0;
  int64_t length = 0;
};
enum class RangeResult { none, ok, unsatisfiable };
// Parses "bytes=a-b", "bytes=a-" and "bytes=-n". Multiple ranges are not supported
// and are treated as "no range" (the whole file is sent).
RangeResult parse_range(std::string_view header, int64_t file_size, ByteRange& out);

// Builds a response for a file on disk, honouring a Range header.
Response file_response(const Request& req, const std::filesystem::path& path,
                       const std::string& content_type);

}  // namespace eos::api
