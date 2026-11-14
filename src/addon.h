#pragma once

#include <string>
#include <vector>

#include <kodi/AddonBase.h>
#include <kodi/addon-instance/PVR.h>

#include "epg.h"
#include "proxy.h"

class ATTR_DLL_LOCAL CInstanceCCTV : public kodi::addon::CInstancePVRClient
{
public:
  explicit CInstanceCCTV(const kodi::addon::IInstanceInfo& instance);
  ~CInstanceCCTV() override = default;

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;

  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;
  PVR_ERROR GetBackendHostname(std::string& hostname) override;
  PVR_ERROR GetConnectionString(std::string& connection) override;

  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;

  PVR_ERROR GetChannelStreamProperties(
      const kodi::addon::PVRChannel& channel,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR GetEPGForChannel(int channelUid,
                             time_t start,
                             time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;

  PVR_ERROR GetEPGTagStreamProperties(
      const kodi::addon::PVREPGTag& tag,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag,
                             bool& isPlayable) override;

private:
  int CatchupDays();
  int EpgCacheMinutes();
  int HttpTimeoutSeconds();

  cctv::EpgRepository m_epg;
  cctv::HlsProxy m_proxy;
};

class ATTR_DLL_LOCAL CAddonCCTV : public kodi::addon::CAddonBase
{
public:
  CAddonCCTV() = default;

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override;
};
