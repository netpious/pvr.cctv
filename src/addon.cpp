#include "addon.h"

#include "channels.h"
#include "stream.h"
#include "timeutil.h"

#include <cstddef>
#include <ctime>
#include <unordered_set>

#include <kodi/General.h>

namespace
{

constexpr const char* kHlsMimeType = "application/vnd.apple.mpegurl";

unsigned int MakeUniqueBroadcastId(int channelUid, time_t start)
{
  // Stable across requests and unique per channel. Kodi uses this value to
  // deduplicate EPG entries.
  return static_cast<unsigned int>(channelUid) * 1000000u +
         static_cast<unsigned int>(static_cast<long long>(start) % 1000000LL);
}

void ProxyLog(const std::string& message)
{
  kodi::Log(ADDON_LOG_INFO, "pvr.cctv proxy: %s", message.c_str());
}

} // namespace

CInstanceCCTV::CInstanceCCTV(const kodi::addon::IInstanceInfo& instance)
  : kodi::addon::CInstancePVRClient(instance)
{
  m_proxy.SetLogger(&ProxyLog);
  kodi::Log(ADDON_LOG_INFO, "pvr.cctv: creating CCTV PVR instance");
}

int CInstanceCCTV::CatchupDays()
{
  // The CCTV web player only offers catch-up for the last 7 days.
  const int days = GetInstanceSettingInt("catchup_days", 7);
  return days < 0 ? 0 : (days > 7 ? 7 : days);
}

int CInstanceCCTV::EpgCacheMinutes()
{
  const int minutes = GetInstanceSettingInt("epg_cache_minutes", 5);
  return minutes < 1 ? 1 : minutes;
}

int CInstanceCCTV::HttpTimeoutSeconds()
{
  const int seconds = GetInstanceSettingInt("http_timeout_seconds", 12);
  return seconds < 3 ? 3 : seconds;
}

PVR_ERROR CInstanceCCTV::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(false);
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsEPGEdl(false);

  capabilities.SetSupportsChannelGroups(false);
  capabilities.SetSupportsChannelScan(false);
  capabilities.SetSupportsChannelSettings(false);
  capabilities.SetSupportsProviders(false);

  capabilities.SetSupportsTimers(false);
  capabilities.SetSupportsRecordings(false);
  capabilities.SetSupportsRecordingsUndelete(false);
  capabilities.SetSupportsRecordingsDelete(false);

  capabilities.SetSupportsDescrambleInfo(false);
  capabilities.SetSupportsAsyncEPGTransfer(false);

  // Kodi opens the HLS URL with ffmpeg; the add-on does not provide a stream
  // pipe.
  capabilities.SetHandlesInputStream(false);
  capabilities.SetHandlesDemuxing(false);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetBackendName(std::string& name)
{
  name = "CCTV";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetBackendVersion(std::string& version)
{
  version = "1.0.0";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetBackendHostname(std::string& hostname)
{
  hostname = "api.cntv.cn";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetConnectionString(std::string& connection)
{
  connection = "https://api.cntv.cn";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetChannelsAmount(int& amount)
{
  amount = static_cast<int>(cctv::kChannelCount);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR;

  for (std::size_t i = 0; i < cctv::kChannelCount; ++i)
  {
    const cctv::ChannelInfo& info = cctv::kChannels[i];

    kodi::addon::PVRChannel channel;
    channel.SetUniqueId(info.uid);
    channel.SetIsRadio(false);
    channel.SetChannelNumber(info.uid);
    channel.SetChannelName(info.name);
    channel.SetHasArchive(true);
    channel.SetMimeType(kHlsMimeType);
    if (info.icon != nullptr && info.icon[0] != '\0')
      channel.SetIconPath(info.icon);

    results.Add(channel);
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  const cctv::ChannelInfo* info = cctv::FindChannelByUid(channel.GetUniqueId());
  if (info == nullptr)
    return PVR_ERROR_INVALID_PARAMETERS;

  // The live streams are per-NAL H5E encrypted (see 06-H5E加密流与高清解密.md),
  // so hand Kodi a local proxy URL that decrypts each segment on the fly.
  // If the proxy cannot start we fall back to the upstream URL.
  std::string streamUrl;
  if (!m_proxy.MakeUrl(info->liveUrl, streamUrl))
    streamUrl = info->liveUrl;

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, kHlsMimeType);
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetEPGForChannel(int channelUid,
                                         time_t start,
                                         time_t end,
                                         kodi::addon::PVREPGTagsResultSet& results)
{
  const cctv::ChannelInfo* info = cctv::FindChannelByUid(static_cast<unsigned int>(channelUid));
  if (info == nullptr)
    return PVR_ERROR_INVALID_PARAMETERS;

  if (start <= 0 || end <= start)
    return PVR_ERROR_NO_ERROR;

  m_epg.SetTodayTtlMinutes(EpgCacheMinutes());
  m_epg.SetHttpTimeoutSeconds(HttpTimeoutSeconds());

  std::unordered_set<unsigned int> seen;
  const std::vector<int> days = cctv::DaysForRange(start, end);

  for (const int day : days)
  {
    bool ok = true;
    const std::vector<cctv::EpgProgram> programs = m_epg.GetPrograms(info->key, day, ok);
    if (!ok)
      continue; // upstream failure: return whatever other days provided

    for (const cctv::EpgProgram& program : programs)
    {
      if (program.end <= start || program.start >= end)
        continue;

      const unsigned int uniqueId =
          MakeUniqueBroadcastId(channelUid, program.start);
      if (!seen.insert(uniqueId).second)
        continue;

      kodi::addon::PVREPGTag tag;
      tag.SetUniqueBroadcastId(uniqueId);
      tag.SetUniqueChannelId(static_cast<unsigned int>(channelUid));
      tag.SetTitle(program.title);
      tag.SetStartTime(program.start);
      tag.SetEndTime(program.end);
      if (!program.columnUrl.empty())
        tag.SetSeriesLink(program.columnUrl);

      results.Add(tag);
    }
  }

  // Always return success. Returning an error for a flaky public endpoint
  // makes Kodi show repeated error dialogs; an empty result is enough and the
  // failure was already logged.
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::GetEPGTagStreamProperties(
    const kodi::addon::PVREPGTag& tag,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  const cctv::ChannelInfo* info =
      cctv::FindChannelByUid(tag.GetUniqueChannelId());
  if (info == nullptr)
    return PVR_ERROR_INVALID_PARAMETERS;

  const time_t now = std::time(nullptr);
  const time_t start = tag.GetStartTime();
  const time_t end = tag.GetEndTime();

  if (start <= 0 || end <= start)
    return PVR_ERROR_NOT_IMPLEMENTED;

  std::string upstream;

  if (now < end)
  {
    // The programme is still on air (or has not started yet). Kodi normally
    // plays those through the channel stream, but it may also route them
    // through the EPG tag, so support that as well.
    if (now < start)
      return PVR_ERROR_NOT_IMPLEMENTED;
    upstream = info->liveUrl;
  }
  else
  {
    const int catchupDays = CatchupDays();
    if (catchupDays <= 0)
      return PVR_ERROR_NOT_IMPLEMENTED;

    if (start < now - static_cast<time_t>(catchupDays) * 86400)
      return PVR_ERROR_NOT_IMPLEMENTED;

    // The catch-up endpoint returns the same classic-grid H5E segments as the
    // live stream, starting at the requested programme. Route it through the
    // decrypting proxy as well.
    upstream = cctv::BuildCatchupUrl(info->liveUrl, start);
  }

  std::string streamUrl;
  if (!m_proxy.MakeUrl(upstream, streamUrl))
    streamUrl = upstream;

  kodi::Log(ADDON_LOG_INFO,
            "pvr.cctv: EPG playback for channel %u starting at %lld (%s)",
            tag.GetUniqueChannelId(), static_cast<long long>(start),
            now < end ? "live" : "catch-up");

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, kHlsMimeType);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CInstanceCCTV::IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag,
                                         bool& isPlayable)
{
  // Kodi asks this while browsing the guide, so it must stay cheap: no proxy
  // start and no network access here. Without this override Kodi assumes that
  // no EPG tag can be played and archive playback is greyed out.
  isPlayable = false;

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.cctv: IsEPGTagPlayable id=%u channel=%u start=%lld end=%lld now=%lld",
            tag.GetUniqueBroadcastId(), tag.GetUniqueChannelId(),
            static_cast<long long>(tag.GetStartTime()),
            static_cast<long long>(tag.GetEndTime()),
            static_cast<long long>(std::time(nullptr)));

  if (cctv::FindChannelByUid(tag.GetUniqueChannelId()) == nullptr)
    return PVR_ERROR_NO_ERROR;

  const time_t now = std::time(nullptr);
  const time_t start = tag.GetStartTime();
  const time_t end = tag.GetEndTime();

  if (start <= 0 || end <= start || start > now)
    return PVR_ERROR_NO_ERROR;

  if (now < end)
  {
    // Currently on air: Kodi plays the live stream.
    isPlayable = true;
    return PVR_ERROR_NO_ERROR;
  }

  const int catchupDays = CatchupDays();
  if (catchupDays <= 0)
    return PVR_ERROR_NO_ERROR;

  isPlayable = start >= now - static_cast<time_t>(catchupDays) * 86400;

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.cctv: IsEPGTagPlayable channel=%u start=%lld end=%lld now=%lld -> %d",
            tag.GetUniqueChannelId(), static_cast<long long>(start),
            static_cast<long long>(end), static_cast<long long>(now),
            isPlayable ? 1 : 0);
  return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS CAddonCCTV::CreateInstance(const kodi::addon::IInstanceInfo& instance,
                                        KODI_ADDON_INSTANCE_HDL& hdl)
{
  if (instance.IsType(ADDON_INSTANCE_PVR))
  {
    hdl = new CInstanceCCTV(instance);
    return ADDON_STATUS_OK;
  }

  return ADDON_STATUS_UNKNOWN;
}

ADDONCREATOR(CAddonCCTV)
