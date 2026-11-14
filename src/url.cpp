#include "url.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace cctv
{
namespace
{

std::string ToLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Trim(const std::string& value)
{
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    --end;
  return value.substr(begin, end - begin);
}

int HexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Removes "." and ".." segments and empty duplicate slashes.
std::string NormalizePath(const std::string& path)
{
  const bool absolute = !path.empty() && path[0] == '/';
  const bool trailingSlash = !path.empty() && path.back() == '/';

  std::vector<std::string> parts;
  std::string current;
  for (const char c : path)
  {
    if (c == '/')
    {
      if (!current.empty())
      {
        parts.push_back(current);
        current.clear();
      }
    }
    else
    {
      current.push_back(c);
    }
  }
  if (!current.empty())
    parts.push_back(current);

  std::vector<std::string> out;
  for (const std::string& part : parts)
  {
    if (part == ".")
      continue;
    if (part == "..")
    {
      if (!out.empty())
        out.pop_back();
      continue;
    }
    out.push_back(part);
  }

  std::string result = absolute ? "/" : "";
  for (std::size_t i = 0; i < out.size(); ++i)
  {
    if (i > 0)
      result.push_back('/');
    result += out[i];
  }
  if (trailingSlash && (result.empty() || result.back() != '/'))
    result.push_back('/');
  if (result.empty())
    result = "/";
  return result;
}

} // namespace

ParsedUrl ParseUrl(const std::string& url)
{
  ParsedUrl out;

  const std::size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos)
    return out;

  out.scheme = ToLower(url.substr(0, schemeEnd));
  if (out.scheme != "http" && out.scheme != "https")
    return out;

  const std::size_t authorityStart = schemeEnd + 3;
  const std::size_t pathStart = url.find_first_of("/?#", authorityStart);
  out.authority = url.substr(
      authorityStart,
      pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);

  const std::size_t at = out.authority.rfind('@');
  if (at != std::string::npos)
    out.authority.erase(0, at + 1);

  if (out.authority.empty())
    return out;

  std::string hostPart = out.authority;
  std::string portPart;

  if (!hostPart.empty() && hostPart[0] == '[')
  {
    const std::size_t close = hostPart.find(']');
    if (close == std::string::npos)
      return out;
    out.host = hostPart.substr(1, close - 1);
    if (close + 1 < hostPart.size() && hostPart[close + 1] == ':')
      portPart = hostPart.substr(close + 2);
  }
  else
  {
    const std::size_t colon = hostPart.rfind(':');
    if (colon != std::string::npos)
    {
      out.host = hostPart.substr(0, colon);
      portPart = hostPart.substr(colon + 1);
    }
    else
    {
      out.host = hostPart;
    }
  }

  if (out.host.empty())
    return out;

  if (!portPart.empty())
  {
    int port = 0;
    for (const char c : portPart)
    {
      if (c < '0' || c > '9')
        return out;
      port = port * 10 + (c - '0');
      if (port > 65535)
        return out;
    }
    out.port = static_cast<unsigned short>(port);
  }
  else
  {
    out.port = out.scheme == "https" ? 443 : 80;
  }

  if (pathStart == std::string::npos)
    out.target = "/";
  else if (url[pathStart] == '/')
    out.target = url.substr(pathStart);
  else
    out.target = "/" + url.substr(pathStart);

  // Fragments are never sent to the server.
  const std::size_t hash = out.target.find('#');
  if (hash != std::string::npos)
    out.target.erase(hash);

  out.valid = true;
  return out;
}

std::string ResolveUrl(const std::string& base, const std::string& ref)
{
  const std::string reference = Trim(ref);
  if (reference.empty())
    return base;

  if (reference.find("://") != std::string::npos)
    return reference;

  const ParsedUrl parsed = ParseUrl(base);
  if (!parsed.valid)
    return reference;

  const std::string prefix = parsed.scheme + "://" + parsed.authority;

  if (reference.size() >= 2 && reference[0] == '/' && reference[1] == '/')
    return parsed.scheme + ":" + reference;

  if (reference[0] == '/')
    return prefix + reference;

  // Split the base target into path and query.
  const std::size_t queryPos = parsed.target.find('?');
  const std::string basePath =
      queryPos == std::string::npos ? parsed.target : parsed.target.substr(0, queryPos);

  if (reference[0] == '?')
    return prefix + basePath + reference;

  if (reference[0] == '#')
    return prefix + basePath + (queryPos == std::string::npos ? "" : parsed.target.substr(queryPos));

  std::string dir;
  const std::size_t slash = basePath.rfind('/');
  if (slash != std::string::npos)
    dir = basePath.substr(0, slash + 1);
  else
    dir = "/";

  const std::size_t refQuery = reference.find('?');
  const std::string refPath =
      refQuery == std::string::npos ? reference : reference.substr(0, refQuery);
  const std::string suffix =
      refQuery == std::string::npos ? "" : reference.substr(refQuery);

  return prefix + NormalizePath(dir + refPath) + suffix;
}

std::string PercentEncode(const std::string& value)
{
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);

  for (const unsigned char c : value)
  {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                            c == '~';
    if (unreserved)
    {
      out.push_back(static_cast<char>(c));
    }
    else
    {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0F]);
      out.push_back(kHex[c & 0x0F]);
    }
  }

  return out;
}

bool PercentDecode(const std::string& value, std::string& out)
{
  out.clear();
  out.reserve(value.size());

  for (std::size_t i = 0; i < value.size(); ++i)
  {
    if (value[i] == '%')
    {
      if (i + 2 >= value.size())
        return false;
      const int hi = HexValue(value[i + 1]);
      const int lo = HexValue(value[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    }
    else
    {
      out.push_back(value[i]);
    }
  }

  return true;
}

} // namespace cctv
