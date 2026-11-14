#pragma once

#include <string>
#include <vector>

namespace cctv
{

class HttpClient
{
public:
  // Performs a blocking GET. Returns false on transport/HTTP error and fills
  // `error` with a human-readable message. TLS verification is kept enabled.
  static bool Get(const std::string& url,
                  const std::vector<std::string>& headers,
                  int timeoutSeconds,
                  std::string& body,
                  std::string& error);
};

} // namespace cctv
