#include "api/http.h"

#include <cctype>
#include <charconv>
#include <format>

#include "core/strings.h"

namespace eos::api {

std::optional<std::string> Request::param(std::string_view name) const {
  for (const auto& [k, v] : params)
    if (k == name) return v;
  return std::nullopt;
}

std::vector<std::string> Request::params_all(std::string_view name) const {
  std::vector<std::string> out;
  for (const auto& [k, v] : params)
    if (k == name) out.push_back(v);
  return out;
}

std::optional<std::string> Request::header(std::string_view lower_name) const {
  auto it = headers.find(std::string(lower_name));
  if (it == headers.end()) return std::nullopt;
  return it->second;
}

std::string url_decode(std::string_view s, bool plus_is_space) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '+' && plus_is_space) {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      int v = 0;
      std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
      out.push_back(static_cast<char>(v));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void parse_query(std::string_view query, std::vector<std::pair<std::string, std::string>>& out) {
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string_view::npos) amp = query.size();
    auto part = query.substr(pos, amp - pos);
    if (!part.empty()) {
      size_t eq = part.find('=');
      if (eq == std::string_view::npos)
        out.emplace_back(url_decode(part), "");
      else
        out.emplace_back(url_decode(part.substr(0, eq)), url_decode(part.substr(eq + 1)));
    }
    pos = amp + 1;
  }
}

RangeResult parse_range(std::string_view header, int64_t size, ByteRange& out) {
  auto h = trim(header);
  if (!starts_with_ci(h, "bytes=")) return RangeResult::none;
  auto spec = trim(std::string_view(h).substr(6));
  if (spec.find(',') != std::string::npos) return RangeResult::none;
  auto dash = spec.find('-');
  if (dash == std::string::npos) return RangeResult::none;
  auto a = trim(std::string_view(spec).substr(0, dash));
  auto b = trim(std::string_view(spec).substr(dash + 1));
  auto to_int = [](const std::string& s, int64_t& v) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && p == s.data() + s.size() && v >= 0;
  };
  int64_t start = 0, end = 0;
  if (a.empty()) {  // suffix range: last n bytes
    int64_t n;
    if (!to_int(b, n)) return RangeResult::none;
    if (n == 0 || size == 0) return RangeResult::unsatisfiable;
    start = std::max<int64_t>(0, size - n);
    end = size - 1;
  } else {
    if (!to_int(a, start)) return RangeResult::none;
    if (b.empty()) {
      end = size - 1;
    } else if (!to_int(b, end) || end < start) {
      return RangeResult::none;
    }
    if (start >= size) return RangeResult::unsatisfiable;
    end = std::min(end, size - 1);
  }
  out.start = start;
  out.length = end - start + 1;
  return RangeResult::ok;
}

Response file_response(const Request& req, const std::filesystem::path& path,
                       const std::string& content_type) {
  Response r;
  std::error_code ec;
  auto size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec) {
    r.status = 404;
    r.body = "file not found";
    return r;
  }
  r.content_type = content_type;
  r.file = path;
  r.headers.emplace_back("Accept-Ranges", "bytes");
  ByteRange range;
  auto rr = req.header("range") ? parse_range(*req.header("range"), size, range) : RangeResult::none;
  if (rr == RangeResult::unsatisfiable) {
    r.file.reset();
    r.status = 416;
    r.headers.emplace_back("Content-Range", std::format("bytes */{}", size));
    return r;
  }
  if (rr == RangeResult::ok) {
    r.status = 206;
    r.offset = range.start;
    r.length = range.length;
    r.headers.emplace_back(
        "Content-Range", std::format("bytes {}-{}/{}", range.start, range.start + range.length - 1, size));
  } else {
    r.offset = 0;
    r.length = size;
  }
  return r;
}

}  // namespace eos::api
