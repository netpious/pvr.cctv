#include "h5e.h"

#include <algorithm>
#include <cstring>

namespace cctv
{
namespace
{

constexpr std::uint32_t kDelta = 0x9E3779B9u;

// Classic / new-grid stride bases. The stride is BASE[idx] | key[idx].
// These exceed 255, so they must not be stored in uint8_t.
constexpr std::uint32_t kBase[6] = {160, 192, 224, 256, 288, 320};

// NALs shorter than this are usually parameter sets / SEI and are never
// encrypted by the type-1 grid.
constexpr std::size_t kMinType1 = 129;

// Cells must stop this many bytes before the end of the NAL.
constexpr std::size_t kGuard = 17;

// Fixed feedback pattern of the G transform.
constexpr std::uint8_t kBitStep[16] = {0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0};

inline std::uint32_t Read32(const std::uint8_t* p)
{
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void Write32(std::uint8_t* p, std::uint32_t v)
{
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

bool HasEmulationPrevention(const std::uint8_t* p, std::size_t n)
{
  for (std::size_t i = 0; i + 2 < n; ++i)
  {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3)
      return true;
  }
  return false;
}

// Removes every 00 00 03 -> 00 00. All grid offsets are expressed in this
// RBSP space, which is the single most common source of "works for a few
// frames, then turns into garbage" bugs.
std::vector<std::uint8_t> ToRbsp(const std::uint8_t* p, std::size_t n)
{
  std::vector<std::uint8_t> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3)
    {
      out.push_back(0);
      out.push_back(0);
      i += 2;
    }
    else
    {
      out.push_back(p[i]);
    }
  }
  return out;
}

void TeaDecrypt(std::uint8_t* p, const std::uint32_t key[4])
{
  std::uint32_t v0 = Read32(p);
  std::uint32_t v1 = Read32(p + 4);
  std::uint32_t sum = kDelta * 16u;

  for (int i = 0; i < 16; ++i)
  {
    v1 -= ((v0 << 4) + key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + key[3]);
    v0 -= ((v1 << 4) + key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + key[1]);
    sum -= kDelta;
  }

  Write32(p, v0);
  Write32(p + 4, v1);
}

std::uint32_t StrideOf(const std::uint8_t* key6)
{
  const std::uint32_t idx = Read32(key6) % 6u;
  return static_cast<std::uint32_t>(kBase[idx]) | key6[idx];
}

// Single feedback bit of the G transform.
inline std::uint32_t fbit(std::uint32_t w)
{
  const std::uint32_t w0 = w & 1u;
  const std::uint32_t w8 = (w >> 8) & 1u;
  const std::uint32_t w15 = (w >> 15) & 1u;
  const std::uint32_t w19 = (w >> 19) & 1u;
  const std::uint32_t w25 = (w >> 25) & 1u;
  const std::uint32_t w30 = (w >> 30) & 1u;
  const std::uint32_t w31 = (w >> 31) & 1u;
  const std::uint32_t t = w0 | w8;

  return (w31 ^ w15 ^ t ^ (w8 & w19) ^ (w25 & (w0 ^ w19)) ^ (w0 & (1u ^ w8) & w30) ^
          ((1u ^ w0) & w19 & w30) ^ (w25 & w30 & (w8 ^ w19))) &
         1u;
}

// 16-bit flip mask selected by the first three bytes of the RBSP.
std::uint32_t FlipMask(const std::uint8_t* hdr)
{
  const std::uint8_t b0 = hdr[0];
  const std::uint8_t b1 = hdr[1];
  const std::uint8_t b2 = hdr[2];
  std::uint32_t m = 0;

  if (b0 == 0x01 && b1 == 0xA8)
  {
    if ((b2 >> 7) & 1) m |= 1u << 0;
    if ((b2 >> 6) & 1) m |= 1u << 1;
    if (!((b2 >> 5) & 1)) m |= 1u << 2;
    if ((b2 >> 4) & 1) m |= 1u << 3;
    if ((b2 >> 3) & 1) m |= 1u << 4;
    if ((b2 >> 1) & 1) m |= 1u << 6;
    if ((b2 >> 0) & 1) m |= 1u << 7;
    m |= 1u << 9;
    m |= 1u << 12;
    return m;
  }

  if (b0 == 0x61)
  {
    if ((b2 >> 1) & 1) m |= 1u << 0;
    if ((b2 >> 0) & 1) m |= 1u << 1;
    if (!((b2 >> 5) & 1)) m |= 1u << 2;
    if ((b2 >> 3) & 1) m |= 1u << 4;
    if ((b2 >> 2) & 1) m |= 1u << 5;
    if ((b2 >> 1) & 1) m |= 1u << 6;
    if ((b2 >> 0) & 1) m |= 1u << 7;
    if ((b2 >> 3) & 1) m |= 1u << 14;
    if ((b2 >> 2) & 1) m |= 1u << 15;
    return m;
  }

  if ((b0 & 0x1F) == 1 && (b1 & 0xF0) == 0x90)
  {
    const std::uint32_t t = (b1 >> 2) & 1u;
    if ((b2 >> 7) & 1) m |= 1u << 0;
    if ((b2 >> 6) & 1) m |= 1u << 1;
    const std::uint32_t s2 =
        t ? ((b0 & 1u) ^ ((b2 >> 5) & 1u)) : (1u ^ ((b2 >> 5) & 1u));
    if (s2) m |= 1u << 2;
    if ((b2 >> 4) & 1) m |= 1u << 3;
    if ((b2 >> 3) & 1) m |= 1u << 4;
    if ((b2 >> 2) & 1) m |= 1u << 5;
    if ((b2 >> 1) & 1) m |= 1u << 6;
    if ((b2 >> 0) & 1) m |= 1u << 7;
    if (t)
    {
      if (b0 & 1u)
      {
        for (const unsigned s : {9u, 10u, 11u, 12u, 14u})
          m |= 1u << s;
      }
      m |= 1u << 13;
    }
    else
    {
      for (const unsigned s : {9u, 10u, 11u, 12u, 14u})
        m |= 1u << s;
    }
    if (b1 & 1u) m |= 1u << 15;
    return m;
  }

  if (b0 == 0x41 && b1 == 0x88)
  {
    if ((b2 >> 7) & 1) m |= 1u << 0;
    if ((b2 >> 6) & 1) m |= 1u << 1;
    if (!((b2 >> 5) & 1)) m |= 1u << 2;
    if ((b2 >> 4) & 1) m |= 1u << 3;
    if ((b2 >> 3) & 1) m |= 1u << 4;
    if ((b2 >> 2) & 1) m |= 1u << 5;
    if ((b2 >> 1) & 1) m |= 1u << 6;
    if ((b2 >> 0) & 1) m |= 1u << 7;
    m |= 1u << 9;
    m |= 1u << 10;
    m |= 1u << 12;
    if (b1 & 1u) m |= 1u << 15;
    return m;
  }

  // Unknown header shape: no flips.
  return 0;
}

void GTransformBlock(std::uint8_t* blk, std::uint32_t mask)
{
  const std::uint32_t x = static_cast<std::uint32_t>(blk[0]) | (static_cast<std::uint32_t>(blk[1]) << 8);
  const std::uint32_t y = static_cast<std::uint32_t>(blk[2]) | (static_cast<std::uint32_t>(blk[3]) << 8);
  std::uint32_t w = (x | (y << 16));
  std::uint32_t out = 0;

  for (int s = 0; s < 16; ++s)
  {
    const std::uint32_t b = fbit(w) ^ ((mask >> s) & 1u) ^ kBitStep[s];
    out |= b << (15 - s);
    w = (w << 1) | b;
  }

  blk[0] = static_cast<std::uint8_t>(out & 0xFF);
  blk[1] = static_cast<std::uint8_t>((out >> 8) & 0xFF);
  blk[2] = static_cast<std::uint8_t>(x & 0xFF);
  blk[3] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
}

std::size_t Type5Stride(const std::uint8_t* nal, std::size_t len)
{
  if (len < 11)
    return 0;
  const std::vector<std::uint8_t> body = ToRbsp(nal, len);
  if (body.size() < 11)
    return 0;
  return StrideOf(body.data() + 5);
}

std::size_t Type1Stride(const std::uint8_t* nal, std::size_t len)
{
  if (len < 7)
    return 0;
  const std::vector<std::uint8_t> body = ToRbsp(nal, len);
  if (body.size() < 7)
    return 0;
  return StrideOf(body.data() + 1);
}

std::vector<std::uint8_t> DecryptClassic(const std::uint8_t* nal, std::size_t len)
{
  std::vector<std::uint8_t> unchanged(nal, nal + len);
  if (len < kMinType1)
    return unchanged;

  std::vector<std::uint8_t> body = ToRbsp(nal, len);
  if (body.size() < 112)
    return unchanged;

  const std::uint32_t key[4] = {
      Read32(body.data() + 16), Read32(body.data() + 20), Read32(body.data() + 24),
      Read32(body.data() + 28)};

  std::size_t cells = 0;
  for (std::size_t o = 32; o + 8 <= body.size(); o += 80)
  {
    TeaDecrypt(body.data() + o, key);
    ++cells;
  }

  return cells > 0 ? body : unchanged;
}

std::vector<std::uint8_t> DecryptType5(const std::uint8_t* nal, std::size_t len, std::size_t stride)
{
  std::vector<std::uint8_t> unchanged(nal, nal + len);
  if (len < 21 || stride < 8)
    return unchanged;

  std::vector<std::uint8_t> body = ToRbsp(nal, len);
  if (body.size() < 21)
    return unchanged;

  const std::uint32_t key[4] = {
      Read32(body.data() + 5), Read32(body.data() + 9), Read32(body.data() + 13),
      Read32(body.data() + 17)};

  std::size_t cells = 0;
  for (std::size_t o = 64; o + 8 <= body.size() && o + kGuard <= body.size(); o += stride)
  {
    TeaDecrypt(body.data() + o, key);
    ++cells;
  }

  return cells > 0 ? body : unchanged;
}

std::vector<std::uint8_t> DecryptType1(const std::uint8_t* nal,
                                       std::size_t len,
                                       std::size_t stride,
                                       std::size_t start = 64)
{
  std::vector<std::uint8_t> unchanged(nal, nal + len);
  if (stride < 4 || len < kMinType1)
    return unchanged;

  std::vector<std::uint8_t> body = ToRbsp(nal, len);
  if (body.size() < kMinType1)
    return unchanged;

  const std::uint32_t mask = FlipMask(body.data());
  std::size_t cells = 0;
  for (std::size_t o = start; o + kGuard <= body.size() && o + 4 <= body.size(); o += stride)
  {
    GTransformBlock(body.data() + o, mask);
    ++cells;
  }

  return cells > 0 ? body : unchanged;
}

std::size_t FindStartCode(const std::vector<std::uint8_t>& es, std::size_t from)
{
  for (std::size_t i = from; i + 2 < es.size(); ++i)
  {
    if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)
      return i;
  }
  return std::string::npos;
}

std::vector<std::uint8_t> DecryptEs(const std::vector<std::uint8_t>& es, H5eDecoder& decoder)
{
  struct Mark
  {
    std::size_t pos;
    std::size_t scLen;
  };

  std::vector<Mark> marks;
  std::size_t pos = FindStartCode(es, 0);
  while (pos != std::string::npos)
  {
    const bool fourByte = pos > 0 && es[pos - 1] == 0;
    marks.push_back(Mark{fourByte ? pos - 1 : pos, fourByte ? 4u : 3u});
    pos = FindStartCode(es, pos + 3);
  }

  if (marks.empty())
    return es;

  std::vector<std::uint8_t> out;
  out.reserve(es.size());

  for (std::size_t i = 0; i < marks.size(); ++i)
  {
    const std::size_t start = marks[i].pos;
    const std::size_t end = (i + 1 < marks.size()) ? marks[i + 1].pos : es.size();
    out.insert(out.end(), es.begin() + static_cast<std::ptrdiff_t>(start),
               es.begin() + static_cast<std::ptrdiff_t>(start + marks[i].scLen));

    const std::size_t nalStart = start + marks[i].scLen;
    if (nalStart >= end)
      continue;

    const std::vector<std::uint8_t> nal =
        decoder.Feed(es.data() + nalStart, end - nalStart);
    out.insert(out.end(), nal.begin(), nal.end());
  }

  return out;
}

// Offset of the payload inside a 188-byte TS packet, plus its length.
struct PayloadRef
{
  std::size_t off;
  std::size_t payloadOffset;
  std::size_t capacity;
};

bool PacketPayload(const std::vector<std::uint8_t>& data, std::size_t off, PayloadRef& ref)
{
  if (off + 188 > data.size() || data[off] != 0x47)
    return false;

  const std::uint8_t afc = (data[off + 3] >> 4) & 0x03;
  if (afc == 0 || afc == 2)
    return false;

  const std::size_t pi = (afc == 1) ? 4 : 5 + data[off + 4];
  if (pi >= 188)
    return false;

  ref.off = off;
  ref.payloadOffset = pi;
  ref.capacity = 188 - pi;
  return true;
}

bool PcrOf(const std::vector<std::uint8_t>& data, std::size_t off, std::uint8_t pcr[6])
{
  const std::uint8_t afc = (data[off + 3] >> 4) & 0x03;
  if (afc != 2 && afc != 3)
    return false;

  const std::uint8_t afLen = data[off + 4];
  if (afLen < 7)
    return false;
  if (!(data[off + 5] & 0x10))
    return false;

  std::memcpy(pcr, data.data() + off + 6, 6);
  return true;
}

void RebuildPacket(std::uint8_t* pkt,
                   const std::uint8_t* payload,
                   std::size_t len,
                   const std::uint8_t* pcr)
{
  const std::uint8_t pidHi = pkt[1] & 0x1F;
  const std::uint8_t pusi = pkt[1] & 0x40;
  const std::uint8_t pidLo = pkt[2];
  const std::uint8_t cc = pkt[3] & 0x0F;

  if (len >= 184)
  {
    pkt[0] = 0x47;
    pkt[1] = pusi | pidHi;
    pkt[2] = pidLo;
    pkt[3] = 0x10 | cc;
    std::memcpy(pkt + 4, payload, 184);
    return;
  }

  const bool usePcr = pcr != nullptr && len <= 176;
  const std::uint8_t afLen = static_cast<std::uint8_t>(183 - len);

  pkt[0] = 0x47;
  pkt[1] = pusi | pidHi;
  pkt[2] = pidLo;
  pkt[3] = 0x30 | cc;
  pkt[4] = afLen;
  pkt[5] = usePcr ? 0x10 : 0x00;

  std::size_t idx = 6;
  if (usePcr)
  {
    std::memcpy(pkt + idx, pcr, 6);
    idx += 6;
  }

  const std::size_t payloadStart = 5 + afLen;
  if (payloadStart > idx)
    std::memset(pkt + idx, 0xFF, payloadStart - idx);

  if (len > 0)
    std::memcpy(pkt + payloadStart, payload, len);
}

} // namespace

H5eDecoder::H5eDecoder(bool live) : m_live(live), m_should(live) {}

std::vector<std::uint8_t> H5eDecoder::Feed(const std::uint8_t* nal, std::size_t len)
{
  if (nal == nullptr || len == 0)
    return std::vector<std::uint8_t>(nal, nal + len);

  const unsigned int type = nal[0] & 0x1F;
  ++m_nals;

  // Type 25 is the switch that tells the decoder whether (and with which grid)
  // the following slices are encrypted. Live streams have none and force the
  // classic grid.
  if (type == 25 && !m_live)
  {
    m_should = len >= 2 && nal[1] == 1;
    if (len >= 4 && nal[2] == 0x01)
    {
      if (nal[3] == 0x09)
        m_newMode = true;
      else if (nal[3] == 0x06)
        m_newMode = false;
    }
    return std::vector<std::uint8_t>(nal, nal + len);
  }

  if ((type != 1 && type != 5) || !m_should)
    return std::vector<std::uint8_t>(nal, nal + len);

  if (m_live || !m_newMode)
  {
    ++m_slices;
    return DecryptClassic(nal, len);
  }

  if (type == 5)
  {
    const std::size_t stride = Type5Stride(nal, len);
    if (stride < 8)
      return std::vector<std::uint8_t>(nal, nal + len);
    ++m_slices;
    return DecryptType5(nal, len, stride);
  }

  if (len < kMinType1)
    return std::vector<std::uint8_t>(nal, nal + len);

  ++m_slices;
  const std::size_t stride = Type1Stride(nal, len);
  return DecryptType1(nal, len, stride != 0 ? stride : 511);
}

unsigned int H5eVideoPid(const std::string& ts)
{
  const auto* data = reinterpret_cast<const std::uint8_t*>(ts.data());
  const std::size_t size = ts.size();

  unsigned int pmtPid = 0;
  bool havePmt = false;

  for (std::size_t off = 0; off + 188 <= size; off += 188)
  {
    if (data[off] != 0x47)
      continue;

    const unsigned int pid =
        (static_cast<unsigned int>(data[off + 1] & 0x1F) << 8) | data[off + 2];
    const bool pusi = (data[off + 1] & 0x40) != 0;
    const std::uint8_t afc = (data[off + 3] >> 4) & 0x03;
    if (afc == 0 || afc == 2)
      continue;

    const std::size_t pi = (afc == 1) ? 4 : 5 + data[off + 4];
    if (pi >= 188)
      continue;

    const std::size_t payloadLen = 188 - pi;
    const std::uint8_t* payload = data + off + pi;

    if (pid == 0 && pusi)
    {
      const std::size_t ptr = payload[0];
      if (1 + ptr + 12 > payloadLen)
        continue;

      const std::uint8_t* sec = payload + 1 + ptr;
      const std::size_t secAvail = payloadLen - 1 - ptr;
      const std::size_t secLen = ((static_cast<std::size_t>(sec[1] & 0x0F) << 8) | sec[2]) + 3;
      if (secLen > secAvail || secLen < 12)
        continue;

      const std::size_t bodyStart = 8;
      const std::size_t bodyEnd = secLen - 4; // strip CRC32
      if (bodyEnd <= bodyStart)
        continue;

      for (std::size_t j = bodyStart; j + 4 <= bodyEnd; j += 4)
      {
        const unsigned int program =
            (static_cast<unsigned int>(sec[j]) << 8) | sec[j + 1];
        if (program == 0)
          continue;
        pmtPid = (static_cast<unsigned int>(sec[j + 2] & 0x1F) << 8) | sec[j + 3];
        havePmt = true;
        break;
      }
    }
    else if (havePmt && pid == pmtPid && pusi)
    {
      const std::size_t ptr = payload[0];
      if (1 + ptr + 16 > payloadLen)
        continue;

      const std::uint8_t* sec = payload + 1 + ptr;
      const std::size_t secAvail = payloadLen - 1 - ptr;
      const std::size_t secLen = ((static_cast<std::size_t>(sec[1] & 0x0F) << 8) | sec[2]) + 3;
      if (secLen > secAvail || secLen < 16)
        continue;

      const std::size_t programInfoLen =
          (static_cast<std::size_t>(sec[10] & 0x0F) << 8) | sec[11];

      std::size_t k = 12 + programInfoLen;
      while (k + 5 <= secLen - 4)
      {
        const std::uint8_t streamType = sec[k];
        const unsigned int elementaryPid =
            (static_cast<unsigned int>(sec[k + 1] & 0x1F) << 8) | sec[k + 2];
        const std::size_t esInfoLen =
            (static_cast<std::size_t>(sec[k + 3] & 0x0F) << 8) | sec[k + 4];
        if (streamType == 0x1B || streamType == 0x24)
          return elementaryPid;
        k += 5 + esInfoLen;
      }
    }
  }

  return 0x100;
}

H5eResult H5eDecryptTs(const std::string& ts, unsigned int videoPid, bool live)
{
  H5eResult result;
  result.ts = ts;

  if (ts.size() < 376 || static_cast<std::uint8_t>(ts[0]) != 0x47)
    return result;

  const unsigned int pid = videoPid != 0 ? videoPid : H5eVideoPid(ts);
  result.videoPid = pid;

  std::vector<std::uint8_t> buf(ts.begin(), ts.end());
  H5eDecoder decoder(live);

  std::vector<std::vector<PayloadRef>> groups;
  int current = -1;

  for (std::size_t off = 0; off + 188 <= buf.size(); off += 188)
  {
    if (buf[off] != 0x47)
      continue;

    const unsigned int packetPid =
        (static_cast<unsigned int>(buf[off + 1] & 0x1F) << 8) | buf[off + 2];
    if (packetPid != pid)
      continue;

    if (buf[off + 1] & 0x40)
    {
      groups.emplace_back();
      current = static_cast<int>(groups.size()) - 1;
    }

    if (current < 0)
      continue;

    PayloadRef ref{};
    if (!PacketPayload(buf, off, ref))
      continue;
    groups[static_cast<std::size_t>(current)].push_back(ref);
  }

  for (const std::vector<PayloadRef>& group : groups)
  {
    if (group.empty())
      continue;

    std::size_t total = 0;
    for (const PayloadRef& ref : group)
      total += ref.capacity;

    std::vector<std::uint8_t> pes;
    pes.reserve(total);
    for (const PayloadRef& ref : group)
    {
      pes.insert(pes.end(), buf.begin() + static_cast<std::ptrdiff_t>(ref.off + ref.payloadOffset),
                 buf.begin() + static_cast<std::ptrdiff_t>(ref.off + 188));
    }

    if (pes.size() < 10 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1)
      continue;

    const std::size_t headerLen = 9 + pes[8];
    if (headerLen >= pes.size())
      continue;

    const std::vector<std::uint8_t> es(pes.begin() + static_cast<std::ptrdiff_t>(headerLen), pes.end());
    const std::vector<std::uint8_t> decrypted = DecryptEs(es, decoder);

    std::vector<std::uint8_t> newPes(pes.begin(),
                                     pes.begin() + static_cast<std::ptrdiff_t>(headerLen));
    newPes.insert(newPes.end(), decrypted.begin(), decrypted.end());

    // Decryption shortens the ES (the 00 00 03 bytes are gone). The PES header
    // still advertises the old length, which makes the demuxer log
    // "PES packet size mismatch" and drop packets. Rewrite it, keeping 0
    // (unbounded) for streams that use it.
    if (!(pes[4] == 0 && pes[5] == 0))
    {
      const std::size_t declared = newPes.size() > 6 ? newPes.size() - 6 : 0;
      if (declared >= 0x10000)
      {
        newPes[4] = 0;
        newPes[5] = 0;
      }
      else
      {
        newPes[4] = static_cast<std::uint8_t>((declared >> 8) & 0xFF);
        newPes[5] = static_cast<std::uint8_t>(declared & 0xFF);
      }
    }

    std::size_t written = 0;
    for (const PayloadRef& ref : group)
    {
      const std::size_t remaining = newPes.size() - written;
      const std::size_t n = std::min(ref.capacity, remaining);

      std::uint8_t pcr[6];
      const bool havePcr = PcrOf(buf, ref.off, pcr);

      RebuildPacket(buf.data() + ref.off, newPes.data() + written, n, havePcr ? pcr : nullptr);
      written += n;
    }
  }

  result.ts.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
  result.nals = decoder.NalCount();
  result.slices = decoder.SliceCount();
  result.ok = result.slices > 0;
  return result;
}

} // namespace cctv
