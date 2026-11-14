#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cctv
{

// Stateful per-NAL H5E decoder.
//
// The decoder keeps the "should decrypt" / "new grid" switch state across NALs
// and segments, which is why it must not be shared between independent streams.
// See 06-H5E加密流与高清解密.md for the algorithm.
class H5eDecoder
{
public:
  explicit H5eDecoder(bool live = false);

  // Decrypts one NAL (without its start code). Returns the replacement bytes;
  // the result can be shorter than the input because decryption removes the
  // emulation-prevention bytes (00 00 03 -> 00 00) that sat inside the cells.
  std::vector<std::uint8_t> Feed(const std::uint8_t* nal, std::size_t len);

  std::size_t NalCount() const { return m_nals; }
  std::size_t SliceCount() const { return m_slices; }

private:
  bool m_live = false;
  bool m_should = false;
  bool m_newMode = false;
  std::size_t m_nals = 0;
  std::size_t m_slices = 0;
};

struct H5eResult
{
  std::string ts;  // decrypted stream, or the untouched input when not H5E
  bool ok = false; // true when at least one encrypted cell was processed
  unsigned int videoPid = 0;
  std::size_t nals = 0;
  std::size_t slices = 0;
};

// Finds the H.264/HEVC video PID by walking PAT -> PMT. Returns 0x100 if the
// tables are missing (which is what live segments start with in practice).
unsigned int H5eVideoPid(const std::string& ts);

// Decrypts a whole MPEG-TS segment. `videoPid` may be 0 to auto-detect.
// `live` forces the classic grid, which is what the CCTV cdrm live/catch-up
// streams use (they have no type-25 switch NAL).
H5eResult H5eDecryptTs(const std::string& ts, unsigned int videoPid, bool live);

} // namespace cctv
