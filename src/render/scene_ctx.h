// Internal: per-scene render context + drawing primitives. Not part of the public API.
#pragma once
#include "render/fixmath.h"
#include "render/scene_registry.h"

namespace ss { namespace rfx {

struct SceneCtx {
  uint32_t* px = nullptr; int w = 0, h = 0, unit = 0, cx = 0, cy = 0;   // unit = min(w,h)/2: 1.0 normalized length
  int64_t tUs = 0;                  // speed-scaled scene time (microseconds)
  uint32_t seed = 1; uint32_t bg = 0;
  int intens = 128;                 // 0..256
  int hue16 = 0;
  int low = 0, mid = 0, high = 0, onsetE = 0, rms = 0;   // features 0..256
  bool onset = false, silent = true;
  int ex[4] = {0, 0, 0, 0};         // extras in Q8 (value*256)
  int particlePct = 100;
  bool strobeOK = true;             // false => no hard flashes (RenderState.disableStrobing)
  const uint16_t* rad = nullptr;    // polar maps (when sceneNeedsPolar): radius Q8 of unit, angle turns, tunnel depth
  const uint16_t* ang = nullptr;
  const uint16_t* depth = nullptr;
  int32_t* scratch = nullptr; int scratchN = 0;   // renderer-owned work array (>= w + h + w entries)
};

typedef void (*SceneFn)(SceneCtx&);
SceneFn sceneFn(int index);
bool sceneNeedsPolar(int index);
void buildPolar(int w, int h, uint16_t* rad, uint16_t* ang, uint16_t* depth);

inline uint32_t phaseOf(const SceneCtx& c, int periodMs) {
  return (uint32_t)((c.tUs * 65536) / ((int64_t)periodMs * 1000)) & 0xFFFFu;
}
inline void clearTo(SceneCtx& c, uint32_t col) { uint32_t* p = c.px; for (int i = 0, n = c.w * c.h; i < n; ++i) p[i] = col; }
inline void hspan(SceneCtx& c, int y, int x0, int x1, uint32_t col) {   // inclusive
  if ((unsigned)y >= (unsigned)c.h) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= c.w) x1 = c.w - 1;
  uint32_t* p = c.px + (size_t)y * c.w;
  for (int x = x0; x <= x1; ++x) p[x] = col;
}
inline void hspanAdd(SceneCtx& c, int y, int x0, int x1, uint32_t col) {
  if ((unsigned)y >= (unsigned)c.h) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= c.w) x1 = c.w - 1;
  uint32_t* p = c.px + (size_t)y * c.w;
  for (int x = x0; x <= x1; ++x) p[x] = addSat(p[x], col);
}
inline void fillRect(SceneCtx& c, int x0, int y0, int x1, int y1, uint32_t col) {   // inclusive
  if (y0 < 0) y0 = 0;
  if (y1 >= c.h) y1 = c.h - 1;
  for (int y = y0; y <= y1; ++y) hspan(c, y, x0, x1, col);
}
inline void fillRectAdd(SceneCtx& c, int x0, int y0, int x1, int y1, uint32_t col) {
  if (y0 < 0) y0 = 0;
  if (y1 >= c.h) y1 = c.h - 1;
  for (int y = y0; y <= y1; ++y) hspanAdd(c, y, x0, x1, col);
}
inline void disc(SceneCtx& c, int cx, int cy, int r, uint32_t col, bool add) {
  if (r <= 0) { if (add) fillRectAdd(c, cx, cy, cx, cy, col); else fillRect(c, cx, cy, cx, cy, col); return; }
  for (int dy = -r; dy <= r; ++dy) {
    int hw = (int)isqrt32((uint32_t)(r * r - dy * dy));
    if (add) hspanAdd(c, cy + dy, cx - hw, cx + hw, col); else hspan(c, cy + dy, cx - hw, cx + hw, col);
  }
}
// Annulus [r-th, r+th] drawn with additive blend.
inline void ring(SceneCtx& c, int cx, int cy, int r, int th, uint32_t col) {
  int ro = r + th, ri = r - th; if (ri < 0) ri = 0;
  int y0 = cy - ro, y1 = cy + ro; if (y0 < 0) y0 = 0; if (y1 >= c.h) y1 = c.h - 1;
  for (int y = y0; y <= y1; ++y) {
    int dy = y - cy; int ady = dy < 0 ? -dy : dy;
    int xo = (int)isqrt32((uint32_t)(ro * ro - ady * ady));
    int xi = ady < ri ? (int)isqrt32((uint32_t)(ri * ri - ady * ady)) : -1;
    if (xi < 0) hspanAdd(c, y, cx - xo, cx + xo, col);
    else { hspanAdd(c, y, cx - xo, cx - xi, col); hspanAdd(c, y, cx + xi, cx + xo, col); }
  }
}
// Line of half-thickness t (square stamp), additive when add.
inline void line(SceneCtx& c, int x0, int y0, int x1, int y1, int t, uint32_t col, bool add) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (int guard = 0; guard < 8192; ++guard) {
    if (add) fillRectAdd(c, x0 - t, y0 - t, x0 + t, y0 + t, col); else fillRect(c, x0 - t, y0 - t, x0 + t, y0 + t, col);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

}} // namespace
