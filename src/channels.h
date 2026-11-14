#pragma once

#include <cstddef>
#include <string>

namespace cctv
{

struct ChannelInfo
{
  unsigned int uid;  // stable Kodi unique id / channel number (1..19)
  const char* key;   // CCTV EPG channel slug, e.g. "cctv1" (see channels.cpp)
  const char* name;  // display name
  const char* liveUrl;
  const char* icon;  // may be empty
};

extern const ChannelInfo kChannels[];
extern const std::size_t kChannelCount;

const ChannelInfo* FindChannelByUid(unsigned int uid);

// "cctv1,cctv2,...,ipanda" used by the multi-channel EPG endpoint.
std::string AllChannelKeysCsv();

} // namespace cctv
