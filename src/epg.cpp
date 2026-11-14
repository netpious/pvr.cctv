#include "epg.h"

#include "channels.h"
#include "http.h"
#include "json.h"
#include "timeutil.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <kodi/General.h>

namespace cctv
{
namespace
{

constexpr int kFailureTtlSeconds = 60;
constexpr int kFutureTtlMinutes = 30;

std::string ExtractJsonPayload(const std::string& body)
{
  // The endpoint wraps the object in a JSONP callback, e.g. cb({...});
  const std::size_t first = body.find('{');
  const std::size_t last = body.rfind('}');
  if (first == std::string::npos || last == std::string::npos || last <= first)
    return {};
  return body.substr(first, last - first + 1);
}

std::string BuildEpgUrl(int day)
{
  std::string url = "https://api.cntv.cn/epg/getEpgInfoByChannelNew?c=";
  url += AllChannelKeysCsv();
  url += "&serviceId=tvcctv&d=";
  url += FormatDay(day);
  url += "&t=jsonp&cb=cb";
  return url;
}

void SortAndDedupe(std::vector<EpgProgram>& programs)
{
  std::sort(programs.begin(), programs.end(), [](const EpgProgram& left, const EpgProgram& right) {
    if (left.start != right.start)
      return left.start < right.start;
    return left.end < right.end;
  });

  programs.erase(std::unique(programs.begin(), programs.end(),
                             [](const EpgProgram& left, const EpgProgram& right) {
                               return left.start == right.start && left.end == right.end &&
                                      left.title == right.title;
                             }),
                 programs.end());
}

} // namespace

void EpgRepository::SetTodayTtlMinutes(int minutes)
{
  m_todayTtlMinutes.store(std::max(1, minutes));
}

void EpgRepository::SetHttpTimeoutSeconds(int seconds)
{
  m_httpTimeoutSeconds.store(std::max(3, seconds));
}

std::vector<EpgProgram> EpgRepository::GetPrograms(const std::string& channelKey,
                                                   int day,
                                                   bool& ok)
{
  ok = true;

  std::unique_lock<std::mutex> lock(m_mutex);
  for (;;)
  {
    auto it = m_days.find(day);
    if (it != m_days.end() && IsFreshLocked(it->second, day))
    {
      ok = it->second.fetchOk;
      const auto channel = it->second.channels.find(channelKey);
      if (channel == it->second.channels.end())
        return {};
      return channel->second;
    }

    if (m_inFlight.find(day) == m_inFlight.end())
    {
      m_inFlight.insert(day);
      break;
    }

    // Another thread is already fetching this day; wait for it instead of
    // firing a duplicate request.
    m_cv.wait(lock);
  }
  lock.unlock();

  // Clear the in-flight marker and wake waiters no matter how we leave this
  // scope, so a failed allocation or parser exception cannot deadlock the
  // next caller.
  struct InFlightGuard
  {
    EpgRepository& repo;
    int day;
    ~InFlightGuard()
    {
      {
        std::lock_guard<std::mutex> guard(repo.m_mutex);
        repo.m_inFlight.erase(day);
      }
      repo.m_cv.notify_all();
    }
  } inFlightGuard{*this, day};

  DayEntry entry;
  entry.valid = true;
  entry.fetchedAt = std::chrono::steady_clock::now();
  try
  {
    entry.fetchOk = FetchDay(day, entry.channels);
  }
  catch (const std::exception& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.cctv: EPG fetch for %s threw: %s",
              FormatDay(day).c_str(), e.what());
    entry.fetchOk = false;
    entry.channels.clear();
  }
  catch (...)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.cctv: EPG fetch for %s threw an unknown exception",
              FormatDay(day).c_str());
    entry.fetchOk = false;
    entry.channels.clear();
  }

  std::vector<EpgProgram> result;
  std::lock_guard<std::mutex> guard(m_mutex);
  m_days[day] = std::move(entry);
  PruneLocked();

  auto it = m_days.find(day);
  if (it != m_days.end())
  {
    ok = it->second.fetchOk;
    const auto channel = it->second.channels.find(channelKey);
    if (channel != it->second.channels.end())
      result = channel->second;
  }
  else
  {
    ok = false;
  }

  return result;
}

bool EpgRepository::IsFreshLocked(const DayEntry& entry, int day) const
{
  const auto now = std::chrono::steady_clock::now();

  if (!entry.fetchOk)
    return now - entry.fetchedAt < std::chrono::seconds(kFailureTtlSeconds);

  // Historical days never change again. Today's table changes as the day goes
  // on, and future days may still be edited by CCTV.
  const int today = TodayDay();
  if (day < today)
    return true;

  const int ttlMinutes = day == today ? m_todayTtlMinutes.load() : kFutureTtlMinutes;
  return now - entry.fetchedAt < std::chrono::minutes(ttlMinutes);
}

bool EpgRepository::FetchDay(int day, std::map<std::string, std::vector<EpgProgram>>& out)
{
  const std::string url = BuildEpgUrl(day);
  const std::vector<std::string> headers = {
      "Referer: https://tv.cctv.com/epg/",
  };

  std::string body;
  std::string error;
  if (!HttpClient::Get(url, headers, m_httpTimeoutSeconds.load(), body, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.cctv: EPG request for %s failed: %s", FormatDay(day).c_str(),
              error.c_str());
    return false;
  }

  const std::string payload = ExtractJsonPayload(body);
  if (payload.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.cctv: EPG response for %s is not JSONP",
              FormatDay(day).c_str());
    return false;
  }

  json::Value root;
  std::string parseError;
  if (!json::Parse(payload, root, parseError))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.cctv: EPG JSON parse error for %s: %s",
              FormatDay(day).c_str(), parseError.c_str());
    return false;
  }

  const json::Value* data = root.Find("data");
  if (!data || !data->IsObject())
  {
    const json::Value* errcode = root.Find("errcode");
    const json::Value* message = root.Find("msg");
    kodi::Log(ADDON_LOG_ERROR, "pvr.cctv: EPG response for %s has no data (errcode=%s msg=%s)",
              FormatDay(day).c_str(), errcode ? errcode->GetString().c_str() : "",
              message ? message->GetString().c_str() : "");
    return false;
  }

  std::map<std::string, std::vector<EpgProgram>> parsed;
  bool anyProgram = false;

  for (std::size_t i = 0; i < kChannelCount; ++i)
  {
    const json::Value* channel = data->Find(kChannels[i].key);
    if (!channel || !channel->IsObject())
      continue;

    const json::Value* list = channel->Find("list");
    if (!list || !list->IsArray())
      continue;

    std::vector<EpgProgram> programs;
    for (const auto& item : list->GetArray())
    {
      if (!item.IsObject())
        continue;

      const json::Value* title = item.Find("title");
      const json::Value* start = item.Find("startTime");
      const json::Value* end = item.Find("endTime");
      if (!title || !start || !end)
        continue;

      EpgProgram program;
      program.start = static_cast<time_t>(start->GetInt64());
      program.end = static_cast<time_t>(end->GetInt64());
      program.title = title->GetString();

      // Ignore malformed rows rather than inserting a zero-length EPG item.
      if (program.start <= 0 || program.end <= program.start || program.title.empty())
        continue;

      const json::Value* column = item.Find("column_url");
      if (column && column->IsString())
        program.columnUrl = column->GetString();

      programs.push_back(std::move(program));
    }

    if (!programs.empty())
    {
      SortAndDedupe(programs);
      parsed.emplace(kChannels[i].key, std::move(programs));
      anyProgram = true;
    }
  }

  if (!anyProgram)
  {
    kodi::Log(ADDON_LOG_WARNING, "pvr.cctv: no programs in EPG response for %s",
              FormatDay(day).c_str());
    return false;
  }

  out = std::move(parsed);
  return true;
}

void EpgRepository::PruneLocked()
{
  if (m_days.size() <= 256)
    return;

  // Compare against a real calendar day 30 days ago; subtracting 30 from
  // YYYYMMDD directly is wrong across month boundaries.
  const int cutoff = DayFromTime(std::time(nullptr) - 30 * 86400);
  for (auto it = m_days.begin(); it != m_days.end() && m_days.size() > 128;)
  {
    if (it->first < cutoff)
      it = m_days.erase(it);
    else
      ++it;
  }
}

} // namespace cctv
