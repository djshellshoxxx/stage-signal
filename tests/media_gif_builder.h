// Test/asset-generator helper: authors GIF89a bytes from scratch (own LZW encoder). Not part of the product build.
#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <string>

namespace ss_gif {

struct Frame {
  int x = 0, y = 0, w = 0, h = 0;          // image rectangle inside the logical screen
  std::vector<uint8_t> idx;                // w*h palette indices
  int delayCs = 10;                        // GCE delay in 1/100 s
  int disposal = 0;                        // 0 none, 1 keep, 2 restore bg, 3 restore previous
  int transparent = -1;                    // transparent palette index or -1
  bool emitGce = true;
  std::vector<uint8_t> localPalette;       // optional RGB triples (size must be power of two entries)
};
struct Spec {
  int w = 0, h = 0;
  std::vector<uint8_t> palette;            // global RGB triples, entries must be a power of two (2..256)
  int bgIndex = 0;
  int loop = -1;                           // -1 none, 0 infinite, n = repeat count (NETSCAPE2.0)
  std::vector<Frame> frames;
  bool trailer = true;
};

inline void put16(std::vector<uint8_t>& o, int v) { o.push_back(v & 255); o.push_back((v >> 8) & 255); }

inline int bitsFor(size_t entries) { int b = 1; while (((size_t)1 << b) < entries) ++b; return b; }

inline std::vector<uint8_t> lzw(const std::vector<uint8_t>& px, int minCode) {
  std::vector<uint8_t> out; uint32_t acc = 0; int nb = 0;
  auto emit = [&](int code, int width) { acc |= (uint32_t)code << nb; nb += width; while (nb >= 8) { out.push_back(acc & 255); acc >>= 8; nb -= 8; } };
  int clear = 1 << minCode, eoi = clear + 1, next = eoi + 1, width = minCode + 1;
  std::map<uint32_t, int> dict;
  emit(clear, width);
  if (px.empty()) { emit(eoi, width); if (nb) out.push_back(acc & 255); return out; }
  int prefix = px[0];
  for (size_t i = 1; i < px.size(); ++i) {
    int c = px[i]; uint32_t key = ((uint32_t)prefix << 8) | (uint32_t)c;
    auto it = dict.find(key);
    if (it != dict.end()) { prefix = it->second; continue; }
    emit(prefix, width);
    if (next == 4096) { emit(clear, width); dict.clear(); next = eoi + 1; width = minCode + 1; }
    else { dict[key] = next; if (next == (1 << width) && width < 12) ++width; ++next; }
    prefix = c;
  }
  emit(prefix, width);
  emit(eoi, width);
  if (nb) out.push_back(acc & 255);
  return out;
}

inline std::vector<uint8_t> build(const Spec& s) {
  std::vector<uint8_t> o;
  const char* sig = "GIF89a"; o.insert(o.end(), sig, sig + 6);
  put16(o, s.w); put16(o, s.h);
  int gbits = bitsFor(s.palette.size() / 3);
  o.push_back((uint8_t)(0x80 | ((gbits - 1) << 4) | (gbits - 1)));
  o.push_back((uint8_t)s.bgIndex); o.push_back(0);
  o.insert(o.end(), s.palette.begin(), s.palette.end());
  size_t gEntries = (size_t)1 << gbits; for (size_t i = s.palette.size() / 3; i < gEntries; ++i) { o.push_back(0); o.push_back(0); o.push_back(0); }
  if (s.loop >= 0) {
    o.push_back(0x21); o.push_back(0xFF); o.push_back(11); const char* n = "NETSCAPE2.0"; o.insert(o.end(), n, n + 11);
    o.push_back(3); o.push_back(1); put16(o, s.loop); o.push_back(0);
  }
  for (const Frame& f : s.frames) {
    if (f.emitGce) {
      o.push_back(0x21); o.push_back(0xF9); o.push_back(4);
      o.push_back((uint8_t)((f.disposal << 2) | (f.transparent >= 0 ? 1 : 0)));
      put16(o, f.delayCs); o.push_back((uint8_t)(f.transparent >= 0 ? f.transparent : 0)); o.push_back(0);
    }
    o.push_back(0x2C); put16(o, f.x); put16(o, f.y); put16(o, f.w); put16(o, f.h);
    int lbits = f.localPalette.empty() ? 0 : bitsFor(f.localPalette.size() / 3);
    o.push_back(f.localPalette.empty() ? 0 : (uint8_t)(0x80 | (lbits - 1)));
    if (!f.localPalette.empty()) { o.insert(o.end(), f.localPalette.begin(), f.localPalette.end()); for (size_t i = f.localPalette.size() / 3; i < ((size_t)1 << lbits); ++i) { o.push_back(0); o.push_back(0); o.push_back(0); } }
    int bits = f.localPalette.empty() ? gbits : lbits; if (bits < 2) bits = 2;
    o.push_back((uint8_t)bits);
    std::vector<uint8_t> data = lzw(f.idx, bits);
    for (size_t p = 0; p < data.size(); p += 255) { size_t n = data.size() - p < 255 ? data.size() - p : 255; o.push_back((uint8_t)n); o.insert(o.end(), data.begin() + p, data.begin() + p + n); }
    o.push_back(0);
  }
  if (s.trailer) o.push_back(0x3B);
  return o;
}

} // namespace ss_gif
