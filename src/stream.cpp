#include "stream.h"

namespace cctv
{

std::string BuildCatchupUrl(const std::string& liveUrl, time_t programStart)
{
  std::string url = liveUrl;

  // ipanda has no ?b=... query, so appending "&begintimeabs=" blindly would
  // produce an invalid URL.
  if (url.find('?') == std::string::npos)
    url.push_back('?');
  else if (!url.empty() && url.back() != '?' && url.back() != '&')
    url.push_back('&');

  url += "begintimeabs=";
  url += std::to_string(static_cast<long long>(programStart) * 1000); // milliseconds!
  return url;
}

} // namespace cctv
