#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace cctv
{

// Local HLS proxy that decrypts CCTV's per-NAL H5E grid on the fly.
//
// Kodi/ffmpeg cannot decode the cdrm live and catch-up streams directly, so
// the add-on exposes them as ordinary HLS on 127.0.0.1:
//
//   /p?u=<master>  fetches the upstream playlist and rewrites every URI to
//                  /p (variant playlists) or /s (segments) on this server.
//   /s?u=<segment> fetches one segment and returns it decrypted.
//
// The URLs are self-describing, so no per-playback session has to be tracked
// and Kodi may (re)open any of them at any time.
class HlsProxy
{
public:
  using LogFn = void (*)(const std::string&);

  HlsProxy() = default;
  ~HlsProxy();

  HlsProxy(const HlsProxy&) = delete;
  HlsProxy& operator=(const HlsProxy&) = delete;

  void SetLogger(LogFn logger);

  // Starts the loopback server on first use and returns a local URL for the
  // given upstream playlist. Returns false if the server could not start.
  bool MakeUrl(const std::string& upstreamUrl, std::string& localUrl);

  void Stop();

private:
  bool EnsureStarted();
  void AcceptLoop();
  void HandleClient(std::uintptr_t client);
  void BuildPlaylist(const std::string& upstream, std::string& out, std::string& error);
  void BuildSegment(const std::string& upstream, std::string& out, std::string& error);
  void Log(const std::string& message);

  LogFn m_logger = nullptr;

  std::mutex m_startMutex;
  std::uintptr_t m_listenSocket = 0;
  unsigned short m_port = 0;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<int> m_active{0};

  std::mutex m_stopMutex;
  std::condition_variable m_stopCv;

  struct CacheEntry
  {
    std::string data;
    std::list<std::string>::iterator order;
  };

  std::mutex m_cacheMutex;
  std::list<std::string> m_cacheOrder;
  std::unordered_map<std::string, CacheEntry> m_cache;
  std::size_t m_cacheBytes = 0;

  std::atomic<unsigned long long> m_decrypted{0};
  std::atomic<unsigned long long> m_decryptFailures{0};
};

} // namespace cctv
