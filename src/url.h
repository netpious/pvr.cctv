#pragma once

#include <string>

namespace cctv
{

struct ParsedUrl
{
  std::string scheme;    // "http" or "https"
  std::string authority; // host[:port] exactly as written
  std::string host;      // without port
  unsigned short port = 0;
  std::string target; // path + "?query"
  bool valid = false;
};

ParsedUrl ParseUrl(const std::string& url);

// Resolves `ref` against `base` (RFC 3986 style), e.g. a relative segment name
// against the media playlist URL, or a root-relative variant against the host.
std::string ResolveUrl(const std::string& base, const std::string& ref);

// Percent-encodes everything outside the unreserved set, so the result is safe
// to embed in a query string.
std::string PercentEncode(const std::string& value);

// Decodes %XX escapes. Returns false when an escape is malformed.
bool PercentDecode(const std::string& value, std::string& out);

} // namespace cctv
