#include "channels.h"

namespace cctv
{

// The table is intentionally static: CCTV has no stable public channel
// directory API, while these 19 keys and HLS endpoints have been stable.
// See 04-频道与节目单数据.md.
//
// `key` is the EPG endpoint's channel slug, not always the website slug:
// CCTV-9 is "cctvjilu" and CCTV-14 is "cctvchild" in the EPG API. Requesting
// "cctv9"/"cctv14" yields an empty entry or a 1001 "params error" response.
const ChannelInfo kChannels[] = {
    {1, "cctv1", "CCTV-1 综合",
     "https://ldncctvwbcdbd.a.bdydns.com/ldncctvwbcd/cdrmldcctv1_1/index.m3u8?b=200-2100", ""},
    {2, "cctv2", "CCTV-2 财经",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv2_1/index.m3u8?b=200-2100", ""},
    {3, "cctv3", "CCTV-3 综艺",
     "https://ldocctvwbcdbd.a.bdydns.com/ldocctvwbcd/cdrmldcctv3_1/index.m3u8?b=200-1200", ""},
    {4, "cctv4", "CCTV-4 中文国际",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv4_1/index.m3u8?b=200-2100", ""},
    {5, "cctv5", "CCTV-5 体育",
     "https://ldcctvwbcdtxy.liveplay.myqcloud.com/ldcctvwbcd/cdrmldcctv5_1/index.m3u8?b=200-2100", ""},
    {6, "cctv5plus", "CCTV-5+ 体育赛事",
     "https://ldcctvwbcdtxy.liveplay.myqcloud.com/ldcctvwbcd/cdrmldcctv5plus_1/index.m3u8?b=200-2100", ""},
    {7, "cctv6", "CCTV-6 电影",
     "https://ldocctvwbcdbd.a.bdydns.com/ldocctvwbcd/cdrmldcctv6_1/index.m3u8?b=200-1200", ""},
    {8, "cctv7", "CCTV-7 国防军事",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv7_1/index.m3u8?b=200-2100", ""},
    {9, "cctv8", "CCTV-8 电视剧",
     "https://ldocctvwbcdbd.a.bdydns.com/ldocctvwbcd/cdrmldcctv8_1/index.m3u8?b=200-1200", ""},
    {10, "cctvjilu", "CCTV-9 纪录",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv9_1/index.m3u8?b=200-2100", ""},
    {11, "cctv10", "CCTV-10 科教",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv10_1/index.m3u8?b=200-2100", ""},
    {12, "cctv11", "CCTV-11 戏曲",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv11_1/index.m3u8?b=200-2100", ""},
    {13, "cctv12", "CCTV-12 社会与法",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv12_1/index.m3u8?b=200-2100", ""},
    {14, "cctv13", "CCTV-13 新闻",
     "https://ldncctvwbcdbd.a.bdydns.com/ldncctvwbcd/cdrmldcctv13_1/index.m3u8?b=200-2100", ""},
    {15, "cctvchild", "CCTV-14 少儿",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv14_1/index.m3u8?b=200-2100", ""},
    {16, "cctv15", "CCTV-15 音乐",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv15_1/index.m3u8?b=200-2100", ""},
    {17, "cctv16", "CCTV-16 奥林匹克",
     "https://ldcctvwbcdtxy.liveplay.myqcloud.com/ldcctvwbcd/cdrmldcctv16_1/index.m3u8?b=200-2100", ""},
    {18, "cctv17", "CCTV-17 农业农村",
     "https://ldocctvwbcdtxy.liveplay.myqcloud.com/ldocctvwbcd/cdrmldcctv17_1/index.m3u8?b=200-2100", ""},
    {19, "ipanda", "熊猫频道",
     "https://hdgcwbcdcnc.v.wscdns.com/hdgcwbcd/cdrmipanda_1/index.m3u8", ""},
};

const std::size_t kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);

const ChannelInfo* FindChannelByUid(unsigned int uid)
{
  for (std::size_t i = 0; i < kChannelCount; ++i)
  {
    if (kChannels[i].uid == uid)
      return &kChannels[i];
  }
  return nullptr;
}

std::string AllChannelKeysCsv()
{
  std::string result;
  result.reserve(kChannelCount * 8);
  for (std::size_t i = 0; i < kChannelCount; ++i)
  {
    if (!result.empty())
      result.push_back(',');
    result += kChannels[i].key;
  }
  return result;
}

} // namespace cctv
