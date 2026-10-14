#include "lighting/sacn.h"
#include <cstring>
#include <cstdio>

namespace ss { namespace sacn {

static const uint8_t kAcnId[12] = {'A','S','C','-','E','1','.','1','7',0,0,0};

Cid makeCid(uint64_t seed) {
  Cid c{}; uint64_t s = seed;
  for (int i = 0; i < 2; ++i) {                   // splitmix64
    s += 0x9E3779B97F4A7C15ULL; uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; z ^= z >> 31;
    for (int j = 0; j < 8; ++j) c[i * 8 + j] = (uint8_t)(z >> (8 * j));
  }
  c[6] = (uint8_t)((c[6] & 0x0F) | 0x40); c[8] = (uint8_t)((c[8] & 0x3F) | 0x80);
  return c;
}

std::string multicastAddress(uint32_t u) {
  if (!validUniverse((long)u)) return std::string();
  char b[32]; std::snprintf(b, sizeof b, "239.255.%u.%u", (unsigned)(u >> 8) & 0xFF, (unsigned)u & 0xFF); return b;
}

static void put16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

size_t buildDataPacket(const SourceInfo& s, const DataParams& d, const uint8_t* slots, size_t n, uint8_t* out, size_t cap) {
  if (!out || !slots || n < 1 || n > kMaxSlots || !validUniverse(d.universe) || s.priority > kMaxPriority) return 0;
  size_t total = kHeaderSize + n;
  if (cap < total) return 0;
  std::memset(out, 0, kHeaderSize);
  put16(out + 0, 0x0010); put16(out + 2, 0x0000);
  std::memcpy(out + 4, kAcnId, 12);
  put16(out + 16, 0x7000 | (unsigned)(total - 16));
  put32(out + 18, 0x00000004);
  std::memcpy(out + 22, s.cid.data(), 16);
  put16(out + 38, 0x7000 | (unsigned)(total - 38));
  put32(out + 40, 0x00000002);
  size_t nl = s.name.size() > kSourceNameBytes - 1 ? kSourceNameBytes - 1 : s.name.size();
  while (nl > 0 && nl < s.name.size() && (((uint8_t)s.name[nl]) & 0xC0) == 0x80) --nl;   // do not split a UTF-8 sequence
  std::memcpy(out + 44, s.name.data(), nl);
  out[108] = s.priority;
  put16(out + 109, d.syncAddress);
  out[111] = d.sequence; out[112] = d.options;
  put16(out + 113, d.universe);
  put16(out + 115, 0x7000 | (unsigned)(total - 115));
  out[117] = 0x02; out[118] = 0xA1;
  put16(out + 119, 0); put16(out + 121, 1); put16(out + 123, (unsigned)(n + 1));
  out[125] = 0x00;
  std::memcpy(out + 126, slots, n);
  return total;
}

static unsigned get16(const uint8_t* p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

bool parseDataPacket(const uint8_t* in, size_t n, ParsedData& o) {
  if (!in || n < kHeaderSize + 1) return false;
  if (get16(in) != 0x0010 || get16(in + 2) != 0 || std::memcmp(in + 4, kAcnId, 12) != 0) return false;
  if (get16(in + 16) != (0x7000u | (unsigned)(n - 16)) || get32(in + 18) != 4) return false;
  if (get16(in + 38) != (0x7000u | (unsigned)(n - 38)) || get32(in + 40) != 2) return false;
  if (get16(in + 115) != (0x7000u | (unsigned)(n - 115)) || in[117] != 0x02 || in[118] != 0xA1 || get16(in + 119) != 0 || get16(in + 121) != 1) return false;
  unsigned cnt = get16(in + 123);
  if (cnt < 1 || cnt > kMaxSlots + 1 || kHeaderSize - 1 + cnt != n) return false;
  std::memcpy(o.cid.data(), in + 22, 16);
  size_t l = 0; while (l < kSourceNameBytes && in[44 + l]) ++l;
  o.name.assign((const char*)in + 44, l);
  o.priority = in[108]; o.p.syncAddress = (uint16_t)get16(in + 109); o.p.sequence = in[111]; o.p.options = in[112];
  o.p.universe = (uint16_t)get16(in + 113); o.startCode = in[125]; o.slotCount = (uint16_t)(cnt - 1); o.slots = in + 126;
  return true;
}

}} // namespace ss::sacn
