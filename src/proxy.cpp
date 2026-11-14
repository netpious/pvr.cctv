#include "proxy.h"

#include "h5e.h"
#include "http.h"
#include "url.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cctv
{
namespace
{

constexpr int kMaxConnections = 24;
constexpr std::size_t kMaxSegmentCacheBytes = 48 * 1024 * 1024;
constexpr std::size_t kMaxSegmentCacheEntries = 96;

#ifdef _WIN32

using RawSocket = SOCKET;
constexpr RawSocket kInvalidSocket = INVALID_SOCKET;

bool InitSockets()
{
  WSADATA data{};
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void CloseSocket(RawSocket socket)
{
  if (socket != kInvalidSocket)
    closesocket(socket);
}

void ShutdownSocket(RawSocket socket)
{
  if (socket != kInvalidSocket)
    shutdown(socket, SD_BOTH);
}

int ReceiveSome(RawSocket socket, char* buffer, int length)
{
  return recv(socket, buffer, length, 0);
}

int SendSome(RawSocket socket, const char* buffer, int length)
{
  return send(socket, buffer, length, 0);
}

void SetSocketTimeout(RawSocket socket, int seconds)
{
  DWORD timeout = static_cast<DWORD>(seconds) * 1000u;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
}

#else

using RawSocket = int;
constexpr RawSocket kInvalidSocket = -1;

bool InitSockets()
{
  return true;
}

void CloseSocket(RawSocket socket)
{
  if (socket != kInvalidSocket)
    close(socket);
}

void ShutdownSocket(RawSocket socket)
{
  if (socket != kInvalidSocket)
    shutdown(socket, SHUT_RDWR);
}

int ReceiveSome(RawSocket socket, char* buffer, int length)
{
  return static_cast<int>(recv(socket, buffer, static_cast<std::size_t>(length), 0));
}

int SendSome(RawSocket socket, const char* buffer, int length)
{
  return static_cast<int>(send(socket, buffer, static_cast<std::size_t>(length), 0));
}

void SetSocketTimeout(RawSocket socket, int seconds)
{
  timeval timeout{};
  timeout.tv_sec = seconds;
  timeout.tv_usec = 0;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

#endif

bool SendAll(RawSocket socket, const char* data, std::size_t size)
{
  std::size_t sent = 0;
  while (sent < size)
  {
    const int chunk = SendSome(socket, data + sent, static_cast<int>(size - sent));
    if (chunk <= 0)
      return false;
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

std::string Trim(const std::string& value)
{
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r'))
    ++begin;
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'))
    --end;
  return value.substr(begin, end - begin);
}

bool QueryParam(const std::string& query, const std::string& name, std::string& value)
{
  std::size_t i = 0;
  while (i <= query.size())
  {
    std::size_t end = query.find('&', i);
    if (end == std::string::npos)
      end = query.size();

    const std::string pair = query.substr(i, end - i);
    const std::size_t equals = pair.find('=');
    if (equals != std::string::npos && pair.compare(0, equals, name) == 0)
    {
      std::string encoded = pair.substr(equals + 1);
      for (char& c : encoded)
      {
        if (c == '+')
          c = ' ';
      }
      return PercentDecode(encoded, value);
    }

    if (end == query.size())
      break;
    i = end + 1;
  }
  return false;
}

bool SendResponse(RawSocket socket,
                  int code,
                  const char* reason,
                  const char* contentType,
                  const std::string& body,
                  bool headOnly)
{
  std::string header = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
  header += "Content-Type: ";
  header += contentType;
  header += "\r\nContent-Length: ";
  header += std::to_string(body.size());
  header += "\r\nCache-Control: no-cache, no-store\r\n";
  header += "Access-Control-Allow-Origin: *\r\n";
  header += "Connection: close\r\n\r\n";

  if (!SendAll(socket, header.data(), header.size()))
    return false;

  if (!headOnly && !body.empty())
    return SendAll(socket, body.data(), body.size());

  return true;
}

const std::vector<std::string>& UpstreamHeaders()
{
  static const std::vector<std::string> headers = {
      "Accept: */*",
      "Referer: https://tv.cctv.com/",
  };
  return headers;
}

} // namespace

HlsProxy::~HlsProxy()
{
  Stop();
}

void HlsProxy::SetLogger(LogFn logger)
{
  m_logger = logger;
}

void HlsProxy::Log(const std::string& message)
{
  if (m_logger != nullptr)
    m_logger(message);
}

bool HlsProxy::MakeUrl(const std::string& upstreamUrl, std::string& localUrl)
{
  if (!EnsureStarted())
    return false;

  localUrl = "http://127.0.0.1:" + std::to_string(m_port) + "/p?u=" +
             PercentEncode(upstreamUrl);
  return true;
}

bool HlsProxy::EnsureStarted()
{
  std::lock_guard<std::mutex> lock(m_startMutex);
  if (m_running.load())
    return true;

  if (!InitSockets())
  {
    Log("socket initialisation failed");
    return false;
  }

  RawSocket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalidSocket)
  {
    Log("socket() failed");
    return false;
  }

  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;

  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
  {
    Log("bind(127.0.0.1) failed");
    CloseSocket(listener);
    return false;
  }

  if (listen(listener, 16) != 0)
  {
    Log("listen() failed");
    CloseSocket(listener);
    return false;
  }

  sockaddr_in bound{};
#ifdef _WIN32
  int boundLength = sizeof(bound);
#else
  socklen_t boundLength = sizeof(bound);
#endif
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0)
  {
    Log("getsockname() failed");
    CloseSocket(listener);
    return false;
  }

  m_listenSocket = static_cast<std::uintptr_t>(listener);
  m_port = ntohs(bound.sin_port);
  m_running.store(true);
  m_thread = std::thread(&HlsProxy::AcceptLoop, this);

  Log("local HLS proxy listening on 127.0.0.1:" + std::to_string(m_port));
  return true;
}

void HlsProxy::Stop()
{
  if (!m_running.exchange(false))
    return;

  const RawSocket listener = static_cast<RawSocket>(m_listenSocket);
  m_listenSocket = static_cast<std::uintptr_t>(kInvalidSocket);
  ShutdownSocket(listener);
  CloseSocket(listener);

  if (m_thread.joinable())
    m_thread.join();

  {
    std::unique_lock<std::mutex> lock(m_stopMutex);
    m_stopCv.wait_for(lock, std::chrono::seconds(5),
                      [this]() { return m_active.load() == 0; });
  }

  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
    m_cacheOrder.clear();
    m_cacheBytes = 0;
  }

  Log("local HLS proxy stopped (decrypted " + std::to_string(m_decrypted.load()) +
      " segments, " + std::to_string(m_decryptFailures.load()) + " fallbacks)");
}

void HlsProxy::AcceptLoop()
{
  const RawSocket listener = static_cast<RawSocket>(m_listenSocket);

  while (m_running.load())
  {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listener, &readSet);

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;

#ifdef _WIN32
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int ready = select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif

    if (ready <= 0)
      continue;

    const RawSocket client = accept(listener, nullptr, nullptr);
    if (client == kInvalidSocket)
    {
      if (!m_running.load())
        break;
      continue;
    }

    if (m_active.load() >= kMaxConnections)
    {
      SendResponse(client, 503, "Service Unavailable", "text/plain", "too many connections", false);
      CloseSocket(client);
      continue;
    }

    m_active.fetch_add(1);
    try
    {
      std::thread(&HlsProxy::HandleClient, this, static_cast<std::uintptr_t>(client)).detach();
    }
    catch (...)
    {
      m_active.fetch_sub(1);
      CloseSocket(client);
    }
  }
}

void HlsProxy::HandleClient(std::uintptr_t clientHandle)
{
  const RawSocket client = static_cast<RawSocket>(clientHandle);

  struct ActiveGuard
  {
    std::atomic<int>& active;
    std::condition_variable& cv;
    ~ActiveGuard()
    {
      active.fetch_sub(1);
      cv.notify_all();
    }
  } guard{m_active, m_stopCv};

  SetSocketTimeout(client, 25);

  std::string request;
  char buffer[4096];
  while (request.size() < 16384)
  {
    const int received = ReceiveSome(client, buffer, static_cast<int>(sizeof(buffer)));
    if (received <= 0)
      break;
    request.append(buffer, static_cast<std::size_t>(received));
    if (request.find("\r\n\r\n") != std::string::npos)
      break;
  }

  const std::size_t lineEnd = request.find("\r\n");
  if (lineEnd == std::string::npos)
  {
    CloseSocket(client);
    return;
  }

  const std::string requestLine = request.substr(0, lineEnd);
  const std::size_t firstSpace = requestLine.find(' ');
  if (firstSpace == std::string::npos)
  {
    SendResponse(client, 400, "Bad Request", "text/plain", "bad request", false);
    CloseSocket(client);
    return;
  }

  const std::size_t secondSpace = requestLine.find(' ', firstSpace + 1);
  const std::string method = requestLine.substr(0, firstSpace);
  const std::string target =
      secondSpace == std::string::npos
          ? requestLine.substr(firstSpace + 1)
          : requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

  const bool headOnly = method == "HEAD";
  if (method != "GET" && !headOnly)
  {
    SendResponse(client, 405, "Method Not Allowed", "text/plain", "method not allowed", false);
    CloseSocket(client);
    return;
  }

  const std::size_t questionMark = target.find('?');
  const std::string path =
      questionMark == std::string::npos ? target : target.substr(0, questionMark);
  const std::string query =
      questionMark == std::string::npos ? std::string() : target.substr(questionMark + 1);

  if (path == "/health")
  {
    SendResponse(client, 200, "OK", "text/plain", "ok", headOnly);
    CloseSocket(client);
    return;
  }

  if (path != "/p" && path != "/s")
  {
    SendResponse(client, 404, "Not Found", "text/plain", "not found", headOnly);
    CloseSocket(client);
    return;
  }

  std::string upstream;
  if (!QueryParam(query, "u", upstream) || upstream.empty())
  {
    SendResponse(client, 400, "Bad Request", "text/plain", "missing u parameter", headOnly);
    CloseSocket(client);
    return;
  }

  std::string body;
  std::string error;

  if (path == "/p")
  {
    BuildPlaylist(upstream, body, error);
    if (!error.empty())
    {
      Log("playlist failed: " + error + " [" + upstream + "]");
      SendResponse(client, 502, "Bad Gateway", "text/plain", error, headOnly);
    }
    else
    {
      SendResponse(client, 200, "OK", "application/vnd.apple.mpegurl", body, headOnly);
    }
  }
  else
  {
    BuildSegment(upstream, body, error);
    if (!error.empty())
    {
      Log("segment failed: " + error + " [" + upstream + "]");
      SendResponse(client, 502, "Bad Gateway", "text/plain", error, headOnly);
    }
    else
    {
      SendResponse(client, 200, "OK", "video/mp2t", body, headOnly);
    }
  }

  CloseSocket(client);
}

void HlsProxy::BuildPlaylist(const std::string& upstream, std::string& out, std::string& error)
{
  std::string text;
  if (!HttpClient::Get(upstream, UpstreamHeaders(), 15, text, error))
    return;

  if (text.find("#EXTM3U") == std::string::npos)
  {
    error = "upstream did not return an m3u8 playlist";
    return;
  }

  const bool isMaster = text.find("#EXT-X-STREAM-INF") != std::string::npos;

  out.clear();
  out.reserve(text.size() * 2);

  std::size_t position = 0;
  while (position < text.size())
  {
    std::size_t lineEnd = text.find('\n', position);
    if (lineEnd == std::string::npos)
      lineEnd = text.size();

    const std::string line = Trim(text.substr(position, lineEnd - position));
    position = lineEnd + 1;

    if (line.empty())
    {
      out.push_back('\n');
      continue;
    }

    if (line[0] == '#')
    {
      out += line;
      out.push_back('\n');
      continue;
    }

    const std::string absolute = ResolveUrl(upstream, line);
    out += isMaster ? "/p?u=" : "/s?u=";
    out += PercentEncode(absolute);
    out.push_back('\n');
  }

  if (!isMaster && out.find("/s?u=") == std::string::npos)
  {
    // A media playlist without any segment is useless; report it instead of
    // handing Kodi an empty window.
    error = "playlist contains no segments";
  }
}

void HlsProxy::BuildSegment(const std::string& upstream, std::string& out, std::string& error)
{
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    const auto it = m_cache.find(upstream);
    if (it != m_cache.end())
    {
      m_cacheOrder.splice(m_cacheOrder.begin(), m_cacheOrder, it->second.order);
      out = it->second.data;
      return;
    }
  }

  std::string raw;
  if (!HttpClient::Get(upstream, UpstreamHeaders(), 25, raw, error))
    return;

  H5eResult result = H5eDecryptTs(raw, 0, true);
  if (!result.ok)
  {
    m_decryptFailures.fetch_add(1);
    if (m_decryptFailures.load() == 1)
      Log("segment did not look like H5E; passing upstream bytes through");
    out = std::move(raw);
  }
  else
  {
    m_decrypted.fetch_add(1);
    out = std::move(result.ts);
  }

  if (out.size() > kMaxSegmentCacheBytes / 4)
    return;

  std::lock_guard<std::mutex> lock(m_cacheMutex);
  auto inserted = m_cache.emplace(upstream, CacheEntry{out, {}});
  if (!inserted.second)
    return;

  m_cacheOrder.push_front(upstream);
  inserted.first->second.order = m_cacheOrder.begin();
  m_cacheBytes += out.size();

  while ((m_cacheBytes > kMaxSegmentCacheBytes || m_cache.size() > kMaxSegmentCacheEntries) &&
         !m_cacheOrder.empty())
  {
    const std::string victim = m_cacheOrder.back();
    m_cacheOrder.pop_back();

    const auto it = m_cache.find(victim);
    if (it != m_cache.end())
    {
      m_cacheBytes -= it->second.data.size();
      m_cache.erase(it);
    }
  }
}

} // namespace cctv
