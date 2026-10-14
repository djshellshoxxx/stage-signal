// Integer/fixed-point helpers for the software renderer (no float in pixel paths => identical on x87/SSE).
#pragma once
#include <cstdint>

namespace ss { namespace rfx {

// Full-cycle phase is 16-bit "turns": 0..65535. sin16/cos16 return Q15 (-32767..32767).
int sin16(uint32_t ph16);
inline int cos16(uint32_t ph16) { return sin16(ph16 + 16384u); }
uint32_t isqrt32(uint32_t v);
uint16_t atan2Turns(int y, int x);           // ~0.3 degree accurate, result 0..65535
uint32_t hash32(uint32_t x);
inline uint32_t hash2(uint32_t a, uint32_t b) { return hash32(a * 0x9E3779B1u ^ hash32(b + 0x7F4A7C15u)); }

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 0x00RRGGBB helpers; q is 0..256.
inline uint32_t mulQ8(uint32_t c, uint32_t q) {
  return ((((c & 0xFF00FFu) * q) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * q) >> 8) & 0x00FF00u);
}
inline uint32_t lerpQ8(uint32_t a, uint32_t b, uint32_t q) { return mulQ8(a, 256u - q) + mulQ8(b, q); }
inline uint32_t addSat(uint32_t a, uint32_t b) {
  uint32_t r = (a & 0xFF) + (b & 0xFF), g = ((a >> 8) & 0xFF) + ((b >> 8) & 0xFF), bl = ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (bl > 255) bl = 255;
  return r | (g << 8) | (bl << 16);
}
inline uint32_t maxRgb(uint32_t a, uint32_t b) {
  uint32_t r0 = (a >> 16) & 255, g0 = (a >> 8) & 255, b0 = a & 255, r1 = (b >> 16) & 255, g1 = (b >> 8) & 255, b1 = b & 255;
  return ((r0 > r1 ? r0 : r1) << 16) | ((g0 > g1 ? g0 : g1) << 8) | (b0 > b1 ? b0 : b1);
}
// NOTE: addSat above packs as B,G,R-agnostic sums; it is channel-symmetric so the layout does not matter.
inline int luma8(uint32_t c) { return (int)((((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8); }

uint32_t hsv(int h16, int s8, int v8);        // h 0..65535 turns, s/v 0..255
// 256-entry palettes.
struct Palette {
  uint32_t c[256];
  void rainbow(int hue16, int v8);             // full hue wheel starting at hue16
  void ramp(int hue16, int sat8);              // dark -> bright of one hue (index = brightness)
  void cyclic(int hue16, int hueSpan16);       // smooth hue sweep with sinusoidal brightness, wraps seamlessly
};

}} // namespace
