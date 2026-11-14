#pragma once

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace cctv
{

// CCTV's EPG is published for China Standard Time (UTC+8). The API returns
// absolute Unix seconds, but the "day" parameter (YYYYMMDD) is a Beijing
// calendar day. Do the day arithmetic explicitly so the add-on works the same
// way on a Kodi box in Beijing, Berlin or New York.
constexpr time_t kBeijingOffsetSeconds = 8 * 3600;

inline void GmtimePortable(time_t value, std::tm& out)
{
#if defined(_MSC_VER)
  gmtime_s(&out, &value);
#else
  gmtime_r(&value, &out);
#endif
}

inline time_t TimegmPortable(std::tm& value)
{
#if defined(_WIN32)
  return _mkgmtime(&value);
#else
  return timegm(&value);
#endif
}

// YYYYMMDD in Beijing time.
inline int DayFromTime(time_t value)
{
  std::tm tm{};
  GmtimePortable(value + kBeijingOffsetSeconds, tm);
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

inline int TodayDay()
{
  return DayFromTime(std::time(nullptr));
}

// UTC epoch of 00:00 Beijing time on the given YYYYMMDD.
inline time_t DayStartUtc(int day)
{
  std::tm tm{};
  tm.tm_year = day / 10000 - 1900;
  tm.tm_mon = (day / 100) % 100 - 1;
  tm.tm_mday = day % 100;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_isdst = 0;
  return TimegmPortable(tm) - kBeijingOffsetSeconds;
}

inline std::string FormatDay(int day)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", day / 10000, (day / 100) % 100,
                day % 100);
  return buffer;
}

// Split a UTC epoch window into the Beijing calendar days it touches.
inline std::vector<int> DaysForRange(time_t start, time_t end)
{
  std::vector<int> days;
  if (start <= 0 || end <= start)
    return days;

  // Kodi normally asks for a few days. Guard against a malformed request
  // turning into thousands of HTTP requests.
  constexpr time_t kMaxWindow = 62 * 86400;
  if (end - start > kMaxWindow)
    end = start + kMaxWindow;

  time_t cursor = DayStartUtc(DayFromTime(start));
  while (cursor < end && days.size() < 64)
  {
    days.push_back(DayFromTime(cursor));
    cursor += 86400;
  }
  return days;
}

} // namespace cctv
