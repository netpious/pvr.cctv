#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace cctv
{

struct EpgProgram
{
  time_t start = 0;
  time_t end = 0;
  std::string title;
  std::string columnUrl;
};

// Fetches and caches one Beijing calendar day of CCTV EPG data.
//
// The upstream endpoint returns a whole day for one or more channels in a
// single request. We therefore fetch all 19 channels at once for a missing
// day, which keeps the first guide refresh to one HTTP request instead of 19.
class EpgRepository
{
public:
  EpgRepository() = default;

  EpgRepository(const EpgRepository&) = delete;
  EpgRepository& operator=(const EpgRepository&) = delete;

  void SetTodayTtlMinutes(int minutes);
  void SetHttpTimeoutSeconds(int seconds);

  // Returns the programs for `channelKey` on `day` (YYYYMMDD, Beijing time).
  // `ok` is false when the upstream request failed and no usable data exists.
  // This function never throws.
  std::vector<EpgProgram> GetPrograms(const std::string& channelKey, int day, bool& ok);

private:
  struct DayEntry
  {
    bool valid = false;
    bool fetchOk = false;
    std::chrono::steady_clock::time_point fetchedAt;
    std::map<std::string, std::vector<EpgProgram>> channels;
  };

  bool FetchDay(int day, std::map<std::string, std::vector<EpgProgram>>& out);
  bool IsFreshLocked(const DayEntry& entry, int day) const;
  void PruneLocked();

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::map<int, DayEntry> m_days;
  std::set<int> m_inFlight;
  std::atomic<int> m_todayTtlMinutes{5};
  std::atomic<int> m_httpTimeoutSeconds{12};
};

} // namespace cctv
