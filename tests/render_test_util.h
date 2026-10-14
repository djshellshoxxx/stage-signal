// Shared helpers for test_render_*.cpp (not a test itself).
#pragma once
#include "test.h"
#include "render/renderer.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace rt {
using namespace ss;

inline uint64_t fnv(const Frame& f) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t p : f.px) { for (int i = 0; i < 4; ++i) { h ^= (p >> (8 * i)) & 0xFF; h *= 1099511628211ULL; } }
  return h;
}
inline double meanLuma(const Frame& f) {
  double s = 0; for (uint32_t p : f.px) s += (((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8;
  return f.px.empty() ? 0 : s / (double)f.px.size();
}
inline FeatureSnapshot feats(float lo, float mid, float hi, float onsetE, bool onset, float rmsDb = -18.0f) {
  FeatureSnapshot f; f.band[0] = lo; f.band[1] = mid; f.band[2] = hi; f.onsetStrength = onsetE; f.onset = onset;
  f.rmsDbMix = rmsDb; f.silent = rmsDb < -80; return f;
}
inline RenderState sceneState(int scene, uint32_t seed, Ns t, const FeatureSnapshot& f) {
  RenderState s; s.a.scene = scene; s.a.params = defaultParams(scene); s.a.seed = seed; s.a.timeNs = t; s.elapsedNs = t; s.features = f; return s;
}
inline int px(const Frame& f, int x, int y) { return (int)(f.px[(size_t)y * f.w + x] & 0xFFFFFF); }
inline int ch(uint32_t c, int sh) { return (int)((c >> sh) & 255); }
inline std::string hex64(uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }
// Frame with 4 vertical stripes R,G,B,W of equal width.
inline Frame stripeFrame(int w, int h) {
  Frame f; f.resize(w, h);
  uint32_t c[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) f.px[(size_t)y * w + x] = c[x * 4 / w];
  return f;
}
inline Frame solidFrame(int w, int h, uint32_t c) { Frame f; f.resize(w, h); for (auto& p : f.px) p = c; return f; }
// Black-background state (brightness 0 scene) so layers can be checked in isolation.
inline RenderState blackState() { RenderState s = sceneState(7, 1, 0, FeatureSnapshot()); s.a.params.brightness = 0; return s; }
} // namespace rt
