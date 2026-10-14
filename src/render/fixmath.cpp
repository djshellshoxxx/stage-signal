#include "render/fixmath.h"
namespace ss { namespace rfx {

namespace {
struct SinTable {
  int16_t t[1025];
  SinTable() {
    // sin(pi/2 * x), x in [0,1], via 9th-order Taylor in Q30 integer math (max err ~6e-6) => bit-exact everywhere.
    static const int64_t a1 = 1686629713LL, a3 = 693598668LL, a5 = 85569306LL, a7 = 5026995LL, a9 = 172272LL;
    int16_t q[257];
    for (int i = 0; i <= 256; ++i) {
      int64_t x = ((int64_t)i << 30) / 256;
      int64_t x2 = (x * x) >> 30;
      int64_t p = a9;
      p = a7 - ((p * x2) >> 30);
      p = a5 - ((p * x2) >> 30);
      p = a3 - ((p * x2) >> 30);
      p = a1 - ((p * x2) >> 30);
      int64_t s = (p * x) >> 30;                // Q30
      int64_t v = (s * 32767 + (1LL << 29)) >> 30;
      if (v > 32767) v = 32767;
      q[i] = (int16_t)v;
    }
    for (int i = 0; i < 1024; ++i) {
      int j = i & 511; int v = j <= 256 ? q[j] : q[512 - j];
      t[i] = (int16_t)(i < 512 ? v : -v);
    }
    t[1024] = t[0];
  }
};
const SinTable& tab() { static const SinTable s; return s; }
} // namespace

int sin16(uint32_t ph) {
  const SinTable& s = tab();
  uint32_t p = ph & 0xFFFF; uint32_t i = p >> 6, f = p & 63;
  int a = s.t[i], b = s.t[i + 1];
  return a + (((b - a) * (int)f) >> 6);
}

uint32_t isqrt32(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
    bit >>= 2;
  }
  return r;
}

static int atanOct(int num, int den) {  // atan(num/den) for 0<=num<=den, den>0, result turns Q16 (0..8192)
  int64_t z = ((int64_t)num << 16) / den;
  int64_t k = 8192 + ((2847 * (65536 - z)) >> 16);
  return (int)((z * k) >> 16);
}
uint16_t atan2Turns(int y, int x) {
  int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  if (!ax && !ay) return 0;
  int a = ay <= ax ? atanOct(ay, ax) : 16384 - atanOct(ax, ay);
  if (x < 0) a = 32768 - a;
  if (y < 0) a = 65536 - a;
  return (uint16_t)(a & 0xFFFF);
}

uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}

uint32_t hsv(int h16, int s, int v) {
  h16 &= 0xFFFF;
  int sector = (h16 * 6) >> 16;                       // 0..5
  int rem = (h16 * 6) - (sector << 16);               // 0..65535 position in sector
  int p = v * (255 - s) / 255;
  int q = v * (255 - (s * rem >> 16)) / 255;
  int t = v * (255 - (s * (65535 - rem) >> 16)) / 255;
  int r, g, b;
  switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void Palette::rainbow(int hue16, int v8) { for (int i = 0; i < 256; ++i) c[i] = hsv(hue16 + i * 256, 255, v8); }
void Palette::ramp(int hue16, int sat8) {
  for (int i = 0; i < 256; ++i) c[i] = hsv(hue16 + (i * 40), sat8 - (i * (sat8 / 3) >> 8) + 0, i);
}
void Palette::cyclic(int hue16, int span) {
  for (int i = 0; i < 256; ++i) {
    int v = 70 + (((sin16(i * 256u) + 32768) * 185) >> 16);
    c[i] = hsv(hue16 + 50000 + (int)(((int64_t)span * sin16(i * 256u + 16384u)) >> 15), 235, v);
  }
}

}} // namespace
