#include "recording/h264.h"
#include <cstring>
namespace ss { namespace h264 {

bool looksAnnexB(const uint8_t* d, size_t n) {
  return (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1) || (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1);
}
std::vector<Nal> splitAnnexB(const uint8_t* d, size_t n) {
  std::vector<Nal> v; size_t i = 0, start = (size_t)-1;
  while (i + 3 <= n) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      if (start != (size_t)-1) { size_t e = i; while (e > start && d[e - 1] == 0) --e; if (e > start) v.push_back({d + start, e - start}); }
      start = i + 3; i += 3;
    } else ++i;
  }
  if (start != (size_t)-1) { size_t e = n; while (e > start && d[e - 1] == 0) --e; if (e > start) v.push_back({d + start, e - start}); }
  return v;
}
bool hasIdr(const uint8_t* d, size_t n) { for (auto& x : splitAnnexB(d, n)) if (x.type() == kIdr) return true; return false; }

bool annexBToAvcc(const uint8_t* d, size_t n, Bytes& out, bool strip, Bytes* sps, Bytes* pps) {
  out.clear(); auto nals = splitAnnexB(d, n); if (nals.empty()) return false;
  for (auto& x : nals) {
    int t = x.type();
    if (t == kSps && sps && sps->empty()) sps->assign(x.p, x.p + x.n);
    if (t == kPps && pps && pps->empty()) pps->assign(x.p, x.p + x.n);
    if (strip && (t == kSps || t == kPps || t == kAud)) continue;
    out.push_back((uint8_t)(x.n >> 24)); out.push_back((uint8_t)(x.n >> 16)); out.push_back((uint8_t)(x.n >> 8)); out.push_back((uint8_t)x.n);
    out.insert(out.end(), x.p, x.p + x.n);
  }
  return true;
}
Bytes buildAvcC(const Bytes& sps, const Bytes& pps) {
  if (sps.size() < 4 || sps.size() > 0xFFFF || pps.empty() || pps.size() > 0xFFFF || (sps[0] & 31) != kSps || (pps[0] & 31) != kPps) return Bytes();
  Bytes b; b.push_back(1); b.push_back(sps[1]); b.push_back(sps[2]); b.push_back(sps[3]);   // version, profile, compat, level
  b.push_back(0xFF);                                                                         // 6 reserved bits + lengthSizeMinusOne = 3
  b.push_back(0xE1); b.push_back((uint8_t)(sps.size() >> 8)); b.push_back((uint8_t)sps.size()); b.insert(b.end(), sps.begin(), sps.end());
  b.push_back(1); b.push_back((uint8_t)(pps.size() >> 8)); b.push_back((uint8_t)pps.size()); b.insert(b.end(), pps.begin(), pps.end());
  return b;
}
bool parseAvcC(const uint8_t* d, size_t n, Bytes& sps, Bytes& pps, int* lenSize) {
  sps.clear(); pps.clear();
  if (n < 7 || d[0] != 1) return false;
  if (lenSize) *lenSize = (d[4] & 3) + 1;
  size_t p = 5; unsigned ns = d[p++] & 31;
  for (unsigned i = 0; i < ns; ++i) {
    if (p + 2 > n) return false; size_t l = ((size_t)d[p] << 8) | d[p + 1]; p += 2; if (p + l > n) return false;
    if (i == 0) sps.assign(d + p, d + p + l);
    p += l;
  }
  if (p >= n) return false; unsigned np = d[p++];
  for (unsigned i = 0; i < np; ++i) {
    if (p + 2 > n) return false; size_t l = ((size_t)d[p] << 8) | d[p + 1]; p += 2; if (p + l > n) return false;
    if (i == 0) pps.assign(d + p, d + p + l);
    p += l;
  }
  return !sps.empty() && !pps.empty();
}
Bytes avcCFromAnnexB(const uint8_t* d, size_t n) {
  Bytes sps, pps, tmp; if (!annexBToAvcc(d, n, tmp, true, &sps, &pps)) return Bytes();
  return buildAvcC(sps, pps);
}
}  // namespace h264

namespace aac {
static const uint32_t kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
Bytes audioSpecificConfig(uint32_t rate, uint32_t ch, int ot) {
  int idx = -1; for (int i = 0; i < 13; ++i) if (kRates[i] == rate) idx = i;
  if (idx < 0 || ch < 1 || ch > 2 || ot < 1 || ot > 30) return Bytes();
  uint16_t v = (uint16_t)((ot << 11) | (idx << 7) | (ch << 3));
  return Bytes{(uint8_t)(v >> 8), (uint8_t)v};
}
bool parseAudioSpecificConfig(const uint8_t* d, size_t n, int& ot, uint32_t& rate, uint32_t& ch) {
  if (n < 2) return false;
  uint16_t v = (uint16_t)((d[0] << 8) | d[1]); ot = v >> 11; int idx = (v >> 7) & 15; ch = (v >> 3) & 15;
  if (idx > 12) return false; rate = kRates[idx]; return ot != 0 && ch != 0;
}
bool stripAdts(const uint8_t* d, size_t n, const uint8_t*& p, size_t& len) {
  if (n < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return false;
  size_t hdr = (d[1] & 1) ? 7 : 9; if (n < hdr) return false; p = d + hdr; len = n - hdr; return true;
}
}  // namespace aac
}  // namespace ss
