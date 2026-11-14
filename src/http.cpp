#include "http.h"

#include "url.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

// Windows keeps its own WinHTTP client: it needs no runtime dependency and is
// verified end to end. Every other platform goes through Kodi's virtual file
// system (CCTV_HTTP_KODI_VFS), because Kodi already owns the TLS stack there
// and on Android there is no libcurl an add-on could link against. The
// standalone smoke test has no Kodi host, so it keeps its own HTTP client.
#if defined(CCTV_HTTP_KODI_VFS)
#include <kodi/Filesystem.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace cctv
{
namespace
{

constexpr std::size_t kMaxResponseBytes = 32 * 1024 * 1024;

#if defined(CCTV_HTTP_KODI_VFS)

const char* kUserAgent = "Mozilla/5.0";
const char* kReferer = "https://tv.cctv.com/";

std::string TrimHeaderValue(const std::string& value)
{
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && (value[begin] == ' ' || value[begin] == '\t'))
    ++begin;
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
    --end;
  return value.substr(begin, end - begin);
}

bool IsHeaderNamed(const std::string& name, const char* lowerCaseName)
{
  const std::size_t length = std::strlen(lowerCaseName);
  if (name.size() != length)
    return false;

  for (std::size_t i = 0; i < length; ++i)
  {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (static_cast<char>(std::tolower(c)) != lowerCaseName[i])
      return false;
  }
  return true;
}

// Kodi's HTTP VFS accepts request headers as URL options
// ("<url>|Referer=<value>&User-Agent=<value>"): option values are
// percent-decoded, and any option name that is not a known Kodi setting is
// forwarded verbatim as a request header. Rebuilding the header list this way
// keeps the call sites unchanged.
std::string BuildVfsPath(const std::string& url, const std::vector<std::string>& headers)
{
  std::string options;
  bool haveAgent = false;
  bool haveReferer = false;

  for (const std::string& header : headers)
  {
    const std::size_t colon = header.find(':');
    if (colon == std::string::npos || colon == 0)
      continue;

    const std::string name = header.substr(0, colon);
    const std::string value = TrimHeaderValue(header.substr(colon + 1));
    if (value.empty())
      continue;

    if (IsHeaderNamed(name, "user-agent"))
      haveAgent = true;
    else if (IsHeaderNamed(name, "referer"))
      haveReferer = true;

    options += "&";
    options += PercentEncode(name);
    options += "=";
    options += PercentEncode(value);
  }

  if (!haveAgent)
  {
    options += "&User-Agent=";
    options += PercentEncode(kUserAgent);
  }
  if (!haveReferer)
  {
    options += "&Referer=";
    options += PercentEncode(kReferer);
  }

  // Kodi would otherwise negotiate gzip and hand us decompressed bytes; the
  // playlist and segment parsers want the upstream body untouched.
  options += "&Accept-Encoding=identity";

  return url + "|" + options.substr(1);
}

#elif defined(_WIN32)

const wchar_t* kUserAgent = L"Mozilla/5.0";

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#ifndef WINHTTP_ACCESS_TYPE_NO_PROXY
#define WINHTTP_ACCESS_TYPE_NO_PROXY 1
#endif

std::wstring Widen(const std::string& value)
{
  if (value.empty())
    return std::wstring();

  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0)
    return std::wstring();

  std::wstring out(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), length);
  return out;
}

std::string LastErrorText(const char* what)
{
  return std::string(what) + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

// One WinHTTP session for the lifetime of the process: opening one per request
// would repeat proxy auto-discovery on every HLS segment.
std::mutex g_sessionMutex;
HINTERNET g_session = nullptr;

HINTERNET GetSession()
{
  std::lock_guard<std::mutex> lock(g_sessionMutex);
  if (g_session != nullptr)
    return g_session;

  const wchar_t* noProxy = WINHTTP_NO_PROXY_NAME;
  const wchar_t* noBypass = WINHTTP_NO_PROXY_BYPASS;
  HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  noProxy, noBypass, 0);
  if (session == nullptr)
    session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, noProxy, noBypass, 0);
  if (session == nullptr)
    session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_NO_PROXY, noProxy, noBypass, 0);

  if (session != nullptr)
  {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  }

  g_session = session;
  return g_session;
}

class WinHandle
{
public:
  WinHandle() = default;
  ~WinHandle()
  {
    if (m_handle != nullptr)
      WinHttpCloseHandle(m_handle);
  }

  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;

  void Reset(HINTERNET handle) { m_handle = handle; }
  HINTERNET Get() const { return m_handle; }
  explicit operator bool() const { return m_handle != nullptr; }

private:
  HINTERNET m_handle = nullptr;
};

#else

const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

std::once_flag g_curlInit;

std::size_t WriteCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata)
{
  const std::size_t total = size * nmemb;
  auto* body = static_cast<std::string*>(userdata);

  if (body->size() + total > kMaxResponseBytes)
    return 0; // abort the transfer

  body->append(data, total);
  return total;
}

class CurlHandle
{
public:
  CurlHandle() : m_handle(curl_easy_init()) {}
  ~CurlHandle()
  {
    if (m_handle)
      curl_easy_cleanup(m_handle);
  }

  CurlHandle(const CurlHandle&) = delete;
  CurlHandle& operator=(const CurlHandle&) = delete;

  CURL* Get() const { return m_handle; }

private:
  CURL* m_handle = nullptr;
};

class CurlHeaders
{
public:
  ~CurlHeaders()
  {
    if (m_list)
      curl_slist_free_all(m_list);
  }

  bool Add(const std::string& header)
  {
    curl_slist* next = curl_slist_append(m_list, header.c_str());
    if (!next)
      return false;
    m_list = next;
    return true;
  }

  curl_slist* Get() const { return m_list; }

private:
  curl_slist* m_list = nullptr;
};

#endif

} // namespace

bool HttpClient::Get(const std::string& url,
                     const std::vector<std::string>& headers,
                     int timeoutSeconds,
                     std::string& body,
                     std::string& error)
{
  body.clear();
  error.clear();

  [[maybe_unused]] const int timeout = std::max(1, timeoutSeconds);

#if defined(CCTV_HTTP_KODI_VFS)

  // Reads through Kodi's HTTP implementation: redirects, TLS verification,
  // the configured proxy and the HTTP cache are all handled by Kodi, which is
  // what makes this work on Linux, macOS and Android without libcurl.
  kodi::vfs::CFile file;
  if (!file.OpenFile(BuildVfsPath(url, headers)))
  {
    error = "cannot open " + url;
    return false;
  }

  const int64_t declaredLength = file.GetLength();
  if (declaredLength > static_cast<int64_t>(kMaxResponseBytes))
  {
    error = "response too large";
    return false;
  }

  char buffer[64 * 1024];
  for (;;)
  {
    const auto read = file.Read(buffer, sizeof(buffer));
    if (read < 0)
    {
      error = "read failed for " + url;
      return false;
    }
    if (read == 0)
      break;

    if (body.size() + static_cast<std::size_t>(read) > kMaxResponseBytes)
    {
      error = "response too large";
      return false;
    }
    body.append(buffer, static_cast<std::size_t>(read));
  }

  if (body.empty())
  {
    error = "empty response from " + url;
    return false;
  }

  return true;

#elif defined(_WIN32)

  const ParsedUrl parsed = ParseUrl(url);
  if (!parsed.valid)
  {
    error = "invalid URL: " + url;
    return false;
  }

  HINTERNET session = GetSession();
  if (session == nullptr)
  {
    error = LastErrorText("WinHttpOpen failed");
    return false;
  }

  WinHandle connect;
  connect.Reset(WinHttpConnect(session, Widen(parsed.host).c_str(), parsed.port, 0));
  if (!connect)
  {
    error = LastErrorText("WinHttpConnect failed");
    return false;
  }

  WinHandle request;
  request.Reset(WinHttpOpenRequest(connect.Get(), L"GET", Widen(parsed.target).c_str(),
                                   nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   parsed.scheme == "https" ? WINHTTP_FLAG_SECURE : 0));
  if (!request)
  {
    error = LastErrorText("WinHttpOpenRequest failed");
    return false;
  }

  const int timeoutMs = timeout * 1000;
  WinHttpSetTimeouts(request.Get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);

  std::wstring headerBlock;
  for (const std::string& header : headers)
  {
    headerBlock += Widen(header);
    headerBlock += L"\r\n";
  }
  headerBlock += L"Accept-Encoding: identity\r\n";

  if (!WinHttpAddRequestHeaders(request.Get(), headerBlock.c_str(),
                                static_cast<DWORD>(-1),
                                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
  {
    error = LastErrorText("WinHttpAddRequestHeaders failed");
    return false;
  }

  if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0))
  {
    error = LastErrorText("WinHttpSendRequest failed");
    return false;
  }

  if (!WinHttpReceiveResponse(request.Get(), nullptr))
  {
    error = LastErrorText("WinHttpReceiveResponse failed");
    return false;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(request.Get(),
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                           WINHTTP_NO_HEADER_INDEX))
  {
    error = LastErrorText("WinHttpQueryHeaders failed");
    return false;
  }

  if (status < 200 || status >= 300)
  {
    error = "HTTP " + std::to_string(status);
    return false;
  }

  for (;;)
  {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.Get(), &available))
    {
      error = LastErrorText("WinHttpQueryDataAvailable failed");
      return false;
    }
    if (available == 0)
      break;

    if (body.size() + available > kMaxResponseBytes)
    {
      error = "response too large";
      return false;
    }

    std::string chunk;
    chunk.resize(available);

    DWORD read = 0;
    if (!WinHttpReadData(request.Get(), chunk.data(), available, &read))
    {
      error = LastErrorText("WinHttpReadData failed");
      return false;
    }
    if (read == 0)
      break;

    body.append(chunk.data(), read);
  }

  return true;

#else

  const ParsedUrl parsed = ParseUrl(url);
  if (!parsed.valid)
  {
    error = "invalid URL: " + url;
    return false;
  }

  std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

  CurlHandle curl;
  if (!curl.Get())
  {
    error = "curl_easy_init failed";
    return false;
  }

  CurlHeaders headerList;
  if (!headerList.Add("User-Agent: " + std::string(kUserAgent)) ||
      !headerList.Add("Accept: application/json, text/javascript, */*; q=0.01") ||
      !headerList.Add("Accept-Language: zh-CN,zh;q=0.9,en;q=0.8"))
  {
    error = "failed to allocate HTTP headers";
    return false;
  }
  for (const auto& header : headers)
  {
    if (!headerList.Add(header))
    {
      error = "failed to allocate HTTP headers";
      return false;
    }
  }

  char curlError[CURL_ERROR_SIZE] = {};

  CURL* handle = curl.Get();
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList.Get());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(timeout));
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE,
                   static_cast<curl_off_t>(kMaxResponseBytes));
  curl_easy_setopt(handle, CURLOPT_PROTOCOLS,
                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
  curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS,
                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));

  const CURLcode result = curl_easy_perform(handle);
  if (result != CURLE_OK)
  {
    error = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return false;
  }

  long httpCode = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpCode);
  if (httpCode < 200 || httpCode >= 300)
  {
    error = "HTTP " + std::to_string(httpCode);
    return false;
  }

  return true;

#endif
}

} // namespace cctv
