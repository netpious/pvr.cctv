// Standalone smoke test for the HLS decrypting proxy. It does not need Kodi:
//
//   pvr_cctv_proxy_smoke <upstream-master-url> <output-dir>
//
// The tool starts the proxy, fetches the rewritten playlist, downloads the
// first segment through the proxy and writes it to <output-dir>/segment.ts.
// Run ffmpeg/ffprobe on that file to validate the decryption end to end.

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "http.h"
#include "proxy.h"
#include "url.h"

namespace
{

void WriteFile(const std::string& path, const std::string& data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::cerr << "usage: " << argv[0] << " <upstream-url> <output-dir>\n";
    return 2;
  }

  const std::string upstream = argv[1];
  const std::string outputDir = argv[2];

  cctv::HlsProxy proxy;
  proxy.SetLogger([](const std::string& message) { std::cout << "[proxy] " << message << "\n"; });

  std::string localUrl;
  if (!proxy.MakeUrl(upstream, localUrl))
  {
    std::cerr << "proxy failed to start\n";
    return 1;
  }

  std::cout << "proxy url: " << localUrl << "\n";

  std::string playlist;
  std::string error;
  if (!cctv::HttpClient::Get(localUrl, {}, 20, playlist, error))
  {
    std::cerr << "playlist fetch failed: " << error << "\n";
    return 1;
  }

  WriteFile(outputDir + "/playlist.m3u8", playlist);
  std::cout << "playlist (" << playlist.size() << " bytes):\n" << playlist << "\n";

  // Pick the first rewritten segment URI.
  std::string segmentPath;
  std::size_t position = 0;
  while (position < playlist.size())
  {
    std::size_t lineEnd = playlist.find('\n', position);
    if (lineEnd == std::string::npos)
      lineEnd = playlist.size();
    std::string line = playlist.substr(position, lineEnd - position);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.rfind("/s?u=", 0) == 0)
    {
      segmentPath = line;
      break;
    }
    if (line.rfind("/p?u=", 0) == 0)
    {
      // Master playlist: follow the first variant once.
      const std::string variantUrl =
          "http://" + cctv::ParseUrl(localUrl).authority + line;
      std::string variant;
      if (!cctv::HttpClient::Get(variantUrl, {}, 20, variant, error))
      {
        std::cerr << "variant fetch failed: " << error << "\n";
        return 1;
      }
      WriteFile(outputDir + "/variant.m3u8", variant);
      std::cout << "variant (" << variant.size() << " bytes):\n" << variant << "\n";

      std::size_t vpos = 0;
      while (vpos < variant.size())
      {
        std::size_t e = variant.find('\n', vpos);
        if (e == std::string::npos)
          e = variant.size();
        std::string vline = variant.substr(vpos, e - vpos);
        if (!vline.empty() && vline.back() == '\r')
          vline.pop_back();
        if (vline.rfind("/s?u=", 0) == 0)
        {
          segmentPath = vline;
          break;
        }
        vpos = e + 1;
      }
      break;
    }
    position = lineEnd + 1;
  }

  if (segmentPath.empty())
  {
    std::cerr << "playlist has no segment\n";
    return 1;
  }

  const std::string segmentUrl = "http://" + cctv::ParseUrl(localUrl).authority + segmentPath;
  std::string segment;
  if (!cctv::HttpClient::Get(segmentUrl, {}, 30, segment, error))
  {
    std::cerr << "segment fetch failed: " << error << "\n";
    return 1;
  }

  WriteFile(outputDir + "/segment.ts", segment);
  std::cout << "segment: " << segment.size() << " bytes -> " << outputDir << "/segment.ts\n";

  return 0;
}
