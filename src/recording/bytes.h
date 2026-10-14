// Little/big-endian byte helpers shared by the container writers and their recovery/parse code (MOD-11).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace ss {
namespace rec {
using Bytes = std::vector<uint8_t>;
inline void put8(Bytes& b, uint32_t v) { b.push_back((uint8_t)v); }
inline void put16(Bytes& b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
inline void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
inline void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
inline void putTag(Bytes& b, const char* t) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)t[i]); }
inline void putBytes(Bytes& b, const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; b.insert(b.end(), q, q + n); }
inline uint32_t get16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
inline uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }
inline void patch32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }
inline bool tagIs(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }
// Sample index -> ns, exact integer arithmetic (no accumulated float error).
inline int64_t samplesToNs(uint64_t samples, uint32_t rate) {
  if (!rate) return 0;
  return (int64_t)((samples / rate) * 1000000000ull + (samples % rate) * 1000000000ull / rate);
}
inline uint64_t nsToSamples(int64_t ns, uint32_t rate) {   // floor, clamps negatives to 0
  if (ns <= 0 || !rate) return 0;
  uint64_t n = (uint64_t)ns;
  return (n / 1000000000ull) * rate + (n % 1000000000ull) * rate / 1000000000ull;
}
// float [-1,1] -> signed 24-bit little-endian PCM (3 bytes). NaN -> 0, out-of-range clamped. Returns true if clamped.
inline bool putPcm24(uint8_t* o, float f) {
  bool clamped = false;
  if (!(f == f)) f = 0.f;
  if (f > 1.f) { f = 1.f; clamped = true; } else if (f < -1.f) { f = -1.f; clamped = true; }
  float s = f * 8388607.0f;
  int32_t v = (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
  o[0] = (uint8_t)(v & 255); o[1] = (uint8_t)((v >> 8) & 255); o[2] = (uint8_t)((v >> 16) & 255);
  return clamped;
}
inline bool putPcm16(uint8_t* o, float f) {
  bool clamped = false;
  if (!(f == f)) f = 0.f;
  if (f > 1.f) { f = 1.f; clamped = true; } else if (f < -1.f) { f = -1.f; clamped = true; }
  float s = f * 32767.0f;
  int32_t v = (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
  o[0] = (uint8_t)(v & 255); o[1] = (uint8_t)((v >> 8) & 255);
  return clamped;
}
} // namespace rec
} // namespace ss
