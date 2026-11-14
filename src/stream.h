#pragma once

#include <ctime>
#include <string>

namespace cctv
{

// Returns the live HLS URL with the playback offset appended.
//
// CCTV catch-up is the live timeshift stream plus:
//   &begintimeabs=<program start in MILLISECONDS>
//
// Seconds are silently accepted by the CDN but fall back to the beginning of
// the DVR window, so this function is deliberately the only place that builds
// the parameter. The URL must point at .../index.m3u8; using ..._1.m3u8
// redirects to a live playlist and ignores the time argument.
std::string BuildCatchupUrl(const std::string& liveUrl, time_t programStart);

} // namespace cctv
