// MOD-06: the 12-scene procedural pack. Each scene is a pure function of (seed, speed-scaled time, features, params).
// Integer math + sine LUT only; no allocation; no static state.
#include "render/scene_ctx.h"

namespace ss { namespace rfx {

namespace {

inline int Q8i(int q) { return q >> 8; }

// ---- 0 spectrum_bars -------------------------------------------------------------------------------------------
void sBars(SceneCtx& c) {
  clearTo(c, c.bg);
  int n = clampi(Q8i(c.ex[0]), 2, 128);
  int gapQ = clampi(c.ex[1], 0, 205);
  bool mirror = c.ex[2] >= 128, caps = c.ex[3] >= 128;
  Palette pal; pal.rainbow(c.hue16, 255);
  int pitch256 = (c.w << 8) / n;
  int maxH = (mirror ? c.h / 2 : c.h) * 90 / 100;
  int pitch = pitch256 >> 8; int gap = (pitch * gapQ) >> 8; if (pitch - gap < 1) gap = pitch - 1; if (gap < 0) gap = 0;
  for (int i = 0; i < n; ++i) {
    int pos = n > 1 ? i * 256 / (n - 1) : 0;
    int amp = pos < 128 ? c.low + (((c.mid - c.low) * pos) >> 7) : c.mid + (((c.high - c.mid) * (pos - 128)) >> 7);
    uint32_t hh = hash2(c.seed, (uint32_t)i);
    uint32_t ph = phaseOf(c, 600 + (int)(hh & 511)) + (hh >> 16);
    int jit = 150 + (((sin16(ph) + 32768) * 106) >> 16);              // 150..256
    amp = 24 + ((amp * 232) >> 8);
    amp = (amp * jit) >> 8;
    amp = (amp * (128 + (c.intens >> 1))) >> 8;
    if (amp > 256) amp = 256;
    int hp = (amp * maxH) >> 8;
    int x0 = (i * pitch256) >> 8, x1 = x0 + pitch - gap - 1;
    uint32_t col = pal.c[(i * 256 / n) & 255];
    if (mirror) {
      fillRect(c, x0, c.cy - hp, x1, c.cy + hp, col);
      if (caps) { hspan(c, c.cy - hp - 2, x0, x1, 0xFFFFFF); hspan(c, c.cy + hp + 2, x0, x1, 0xFFFFFF); }
    } else {
      fillRect(c, x0, c.h - hp, x1, c.h - 1, col);
      if (caps) { fillRect(c, x0, c.h - hp - 4, x1, c.h - hp - 3, 0xFFFFFF); }
    }
  }
}

// ---- 1 waveform_ribbon -----------------------------------------------------------------------------------------
void sRibbon(SceneCtx& c) {
  clearTo(c, c.bg);
  int th = (c.unit * c.ex[0]) >> 9; if (th < 1) th = 1;
  int nr = clampi(Q8i(c.ex[1]), 1, 3);
  int freq = c.ex[2];
  int ampPx = (((c.h * 107) >> 8) * c.ex[3]) >> 8;
  Palette pal; pal.rainbow(c.hue16, 255);
  int wl = 70 + ((c.low * 110) >> 8), wm = 40 + ((c.mid * 70) >> 8), wh = 20 + ((c.high * 40) >> 8);
  wl = (wl * (160 + (c.intens >> 1))) >> 8; wm = (wm * (160 + (c.intens >> 1))) >> 8; wh = (wh * (160 + (c.intens >> 1))) >> 8;
  for (int r = 0; r < nr; ++r) {
    uint32_t t1 = phaseOf(c, 3100 + r * 700), t2 = phaseOf(c, 1700 + r * 330), t3 = phaseOf(c, 900 + r * 170);
    int yoff = ((2 * r - (nr - 1)) * c.unit) >> 3;
    int prevY = 0;
    for (int x = 0; x < c.w; ++x) {
      uint32_t u = (uint32_t)(((int64_t)x << 16) / c.w);
      uint32_t f = (u * (uint32_t)freq) >> 8;
      int s = wl * sin16(f + t1) + wm * sin16(f * 3 + 0u - t2) + wh * sin16(f * 7 + t3);
      int y = c.cy + yoff + (int)(((int64_t)s * ampPx) / (350LL * 32768));
      int ya = (x ? (prevY < y ? prevY : y) : y) - th, yb = (x ? (prevY > y ? prevY : y) : y) + th;
      prevY = y;
      if (ya < 0) ya = 0;
      if (yb >= c.h) yb = c.h - 1;
      uint32_t col = pal.c[((x * 256 / c.w) + r * 70 + (int)(t1 >> 8)) & 255];
      uint32_t* p = c.px + (size_t)ya * c.w + x;
      for (int yy = ya; yy <= yb; ++yy, p += c.w) *p = col;
    }
  }
}

// ---- 2 geometric_orbit -----------------------------------------------------------------------------------------
void sOrbit(SceneCtx& c) {
  clearTo(c, c.bg);
  int n = clampi(Q8i(c.ex[0]), 3, 16);
  bool conn = c.ex[1] >= 128;
  int sz = (c.unit * c.ex[2]) >> 8;
  int rings = clampi(Q8i(c.ex[3]), 1, 3);
  Palette pal; pal.rainbow(c.hue16, 255);
  uint32_t spin = phaseOf(c, 14000);
  int bodyR = (sz * (256 + (c.mid >> 1)) >> 8); if (bodyR < 1) bodyR = 1;
  for (int k = 0; k < rings; ++k) {
    int radius = (c.unit * (30 + k * 26)) / 100;
    radius += (c.unit * c.low * (8 + (c.intens >> 5))) / (256 * 40);
    uint32_t base = (k & 1) ? (0u - spin * (uint32_t)(k + 1)) : spin * (uint32_t)(k + 1);
    int xs[16], ys[16];
    for (int i = 0; i < n; ++i) {
      uint32_t a = base + (uint32_t)(i * 65536 / n) + (uint32_t)k * 3000u;
      xs[i] = c.cx + ((cos16(a) * radius) >> 15);
      ys[i] = c.cy + ((sin16(a) * radius) >> 15);
    }
    int skip = n > 4 ? 2 : 1;
    if (conn) for (int i = 0; i < n; ++i) { int j = (i + skip) % n; line(c, xs[i], ys[i], xs[j], ys[j], 0, mulQ8(pal.c[(i * 256 / n + k * 64) & 255], 180), true); }
    for (int i = 0; i < n; ++i) disc(c, xs[i], ys[i], bodyR, pal.c[(i * 256 / n + k * 64) & 255], true);
  }
  disc(c, c.cx, c.cy, bodyR / 2 + 1 + (c.low * bodyR >> 9), 0xFFFFFF, true);
}

// ---- 3 radial_pulse (polar map) --------------------------------------------------------------------------------
void sRadial(SceneCtx& c) {
  Palette pal; pal.cyclic(c.hue16, 22000);
  int dens = c.ex[0];
  int pulse = (c.low * c.ex[1]) >> 8;                // 0..256
  int off = (int)(phaseOf(c, 2800) >> 8);
  int swirl = c.ex[3];
  uint8_t shade[128];
  int fall = c.ex[2];
  for (int j = 0; j < 128; ++j) { int s = 256 - ((j * 8 * fall) >> 9); shade[j] = (uint8_t)(s < 24 ? 24 : (s > 255 ? 255 : s)); }
  int sw = swirl * 24 >> 8;
  int n = c.w * c.h; uint32_t* p = c.px; const uint16_t* R = c.rad; const uint16_t* A = c.ang;
  int shift = off + (pulse >> 2);
  uint32_t aOff = phaseOf(c, 9000) >> 8;
  for (int i = 0; i < n; ++i) {
    int r = R[i];
    int idx = ((r * dens) >> 8) - shift;
    if (sw) idx += (((int)((A[i] + (aOff << 8)) & 0xFFFF) * sw) >> 16);
    p[i] = mulQ8(pal.c[idx & 255], shade[r >> 3]);
  }
  (void)A;
}

// ---- 4 particle_field ------------------------------------------------------------------------------------------
void sParticles(SceneCtx& c) {
  clearTo(c, c.bg);
  int n = (Q8i(c.ex[0]) * c.particlePct) / 100;
  int sizeQ = c.ex[1];
  int flow = c.ex[2] < 64 ? 64 : c.ex[2];
  int bloom = (c.onsetE * c.ex[3]) >> 8;
  Palette pal; pal.rainbow(c.hue16, 255);
  for (int i = 0; i < n; ++i) {
    uint32_t h1 = hash2(c.seed, (uint32_t)i), h2 = hash32(h1 ^ 0xA5A5u);
    int x0 = (int)(h1 & 0xFFFF) - 32768, y0 = (int)(h1 >> 16) - 32768;
    int64_t periodUs = ((int64_t)(4000 + (h2 & 4095)) * 1000 * 256) / flow;
    uint32_t z = (uint32_t)((c.tUs * 65536) / periodUs + (h2 >> 16)) & 0xFFFF;
    int f = (int)((z * z) >> 16);                    // 0..65535 perspective expansion
    int sx = c.cx + (int)(((int64_t)((x0 * c.unit) >> 15) * f * 3) >> 16);
    int sy = c.cy + (int)(((int64_t)((y0 * c.unit) >> 15) * f * 3) >> 16);
    if (sx < -8 || sy < -8 || sx >= c.w + 8 || sy >= c.h + 8) continue;
    int sz = (int)((z * (uint32_t)sizeQ) >> 23) + ((c.w >= 600) ? 1 : 0) + (bloom >> 7);   // half-size
    int v = 40 + (int)(z >> 8) * 215 / 256;
    uint32_t col = mulQ8(pal.c[(h2 >> 4) & 255], (uint32_t)v + (uint32_t)(bloom >> 2));
    fillRectAdd(c, sx - sz, sy - sz, sx + sz, sy + sz, col);
  }
}

// ---- 5 grid_pulse (strobe) -------------------------------------------------------------------------------------
void sGrid(SceneCtx& c) {
  clearTo(c, c.bg);
  int cols = clampi(Q8i(c.ex[0]), 2, 64);
  int cell = c.w / cols; if (cell < 2) cell = 2;
  int rows = c.h / cell + 1;
  int gx0 = (c.w - cols * cell) / 2, gy0 = (c.h - rows * cell) / 2;
  int gap = (cell * c.ex[1]) >> 10; if (gap > cell / 2 - 1) gap = cell / 2 - 1; if (gap < 0) gap = 0;
  int wave = c.ex[2];
  uint32_t tp = phaseOf(c, 1800);
  int flashK = c.ex[3];
  int flash = c.strobeOK ? (c.onset ? 256 : c.onsetE) : (c.onsetE >> 2);
  flash = (flash * flashK) >> 8;
  uint32_t flashCol = mulQ8(0xFFFFFF, (uint32_t)flash);
  Palette pal; pal.rainbow(c.hue16, 255);
  for (int r = 0; r < rows; ++r) {
    int y0 = gy0 + r * cell;
    int dy = (y0 + cell / 2) - c.cy;
    for (int q = 0; q < cols; ++q) {
      int x0 = gx0 + q * cell;
      int dx = (x0 + cell / 2) - c.cx;
      int d = (int)isqrt32((uint32_t)(dx * dx + dy * dy)) * 256 / (c.unit ? c.unit : 1);
      int s = sin16((uint32_t)((d * wave) >> 2) * 64u - tp);          // d*wave/4 turns... scaled
      int b = (s + 32768) >> 8;                                        // 0..256
      b = 40 + ((b * (140 + (c.low >> 1) + (c.intens >> 3))) >> 8);
      if (b > 256) b = 256;
      uint32_t col = mulQ8(pal.c[(q * 256 / cols + r * 9) & 255], (uint32_t)b);
      if (flash) col = addSat(col, flashCol);
      fillRect(c, x0, y0, x0 + cell - gap - 1, y0 + cell - gap - 1, col);
    }
  }
}

// ---- 6 tunnel (polar map) --------------------------------------------------------------------------------------
void sTunnel(SceneCtx& c) {
  Palette pal; pal.cyclic(c.hue16, 30000);
  int segs = clampi(Q8i(c.ex[0]), 2, 32);
  int twist = c.ex[1];
  int stripes = clampi(Q8i(c.ex[2]), 1, 16);
  int fogK = 128 + (c.ex[3] >> 1);
  uint8_t fog[128];
  for (int j = 0; j < 128; ++j) { int s = 28 + (((j << 3) * fogK) >> 8); fog[j] = (uint8_t)(s > 255 ? 255 : s); }
  uint32_t rot = phaseOf(c, 12000);
  int toff = (int)(c.tUs >> 16) + (c.low >> 6);
  int n = c.w * c.h; uint32_t* p = c.px; const uint16_t* R = c.rad; const uint16_t* A = c.ang; const uint16_t* D = c.depth;
  int shade = 100 + (c.intens >> 1) + (c.mid >> 3);
  for (int i = 0; i < n; ++i) {
    int d = D[i];
    uint32_t a = (uint32_t)(A[i] + rot + (uint32_t)((d >> 4) * twist >> 8)) & 0xFFFF;
    int u = (int)((a * (uint32_t)segs) >> 16);
    int v = ((d * stripes) >> 13) + toff;
    int idx = ((u ^ v) & 1) ? 30 + (v & 31) : 170 + (u & 31);
    p[i] = mulQ8(mulQ8(pal.c[idx & 255], fog[R[i] >> 3]), (uint32_t)shade + 1);
  }
}

// ---- 7 color_field ---------------------------------------------------------------------------------------------
void sColor(SceneCtx& c) {
  uint32_t p = phaseOf(c, 24000);
  int drift = c.ex[0];
  int sat = 110 + ((c.ex[1] * 130) >> 8);
  int v = 150 + (((c.low * c.ex[2]) >> 8) * 100 >> 8) + (c.intens >> 4);
  if (v > 255) v = 255;
  int span = 6000 + (drift * 14);
  int sw = (sin16(p) * span) >> 15, sw2 = (sin16(p + 21845u) * span) >> 15;
  int tilt = (c.ex[3] * 20000) >> 8;
  uint32_t cTL = hsv(c.hue16 + sw, sat, v), cTR = hsv(c.hue16 + tilt + sw2, sat, v);
  uint32_t cBL = hsv(c.hue16 - tilt / 2 + sw2, sat, v * 3 / 4), cBR = hsv(c.hue16 + tilt * 3 / 2 + sw, sat, v * 3 / 4);
  int den = c.h > 1 ? c.h - 1 : 1, wd = c.w > 1 ? c.w - 1 : 1;
  for (int y = 0; y < c.h; ++y) {
    uint32_t t = (uint32_t)(y * 256 / den);
    uint32_t L = lerpQ8(cTL, cBL, t), R = lerpQ8(cTR, cBR, t);
    int r = (int)((L >> 16) & 255) << 16, g = (int)((L >> 8) & 255) << 16, b = (int)(L & 255) << 16;
    int dr = ((((int)((R >> 16) & 255)) << 16) - r) / wd, dg = ((((int)((R >> 8) & 255)) << 16) - g) / wd, db = ((((int)(R & 255)) << 16) - b) / wd;
    uint32_t* row = c.px + (size_t)y * c.w;
    for (int x = 0; x < c.w; ++x) {
      row[x] = ((uint32_t)(r >> 16) << 16) | ((uint32_t)(g >> 16) << 8) | (uint32_t)(b >> 16);
      r += dr; g += dg; b += db;
    }
  }
}

// ---- 8 plasma --------------------------------------------------------------------------------------------------
void sPlasma(SceneCtx& c) {
  int span = 8000 + ((c.ex[2] * 36000) >> 8);
  Palette pal; pal.cyclic(c.hue16, span);
  int32_t* A = c.scratch; int32_t* Dg = c.scratch + c.w;
  int fx = (int)(((int64_t)65536 * c.ex[0]) / ((int64_t)c.unit * 256 + 1) * 3 / 2);   // phase per pixel
  int fd = (fx * (128 + (c.ex[1] >> 1))) >> 8;
  uint32_t t1 = phaseOf(c, 7300), t2 = phaseOf(c, 5100), t3 = phaseOf(c, 9700);
  int cyc = c.ex[3];
  for (int x = 0; x < c.w; ++x) A[x] = sin16((uint32_t)(x * fx) + t1);
  for (int k = 0; k < c.w + c.h; ++k) Dg[k] = sin16((uint32_t)(k * fd) - t3);
  int base = (int)(phaseOf(c, 6000) >> 8) + (c.low >> 2);
  int amp = 160 + (c.intens >> 1);
  for (int y = 0; y < c.h; ++y) {
    int B = sin16((uint32_t)(y * fx * 3 / 4) + t2);
    uint32_t* row = c.px + (size_t)y * c.w;
    const int32_t* dg = Dg + y;
    for (int x = 0; x < c.w; ++x) {
      int s = A[x] + B + dg[x];
      int idx = (((s >> 8) * cyc) >> 8) + base;
      row[x] = mulQ8(pal.c[idx & 255], (uint32_t)amp + 1);
    }
  }
}

// ---- 9 starburst (strobe) --------------------------------------------------------------------------------------
void sStar(SceneCtx& c) {
  clearTo(c, c.bg);
  int n = clampi(Q8i(c.ex[0]), 4, 128);
  int len = (c.unit * 360 / 256 * c.ex[1]) >> 8;     // up to ~1.4 unit
  uint32_t spin = (uint32_t)(((int64_t)phaseOf(c, 16000) * c.ex[2]) >> 8);
  Palette pal; pal.rainbow(c.hue16, 255);
  int r0 = c.unit / 14;
  int pump = 200 + (c.low >> 2) + (c.mid >> 3);
  for (int i = 0; i < n; ++i) {
    uint32_t a = spin + (uint32_t)(i * 65536 / n);
    uint32_t hh = hash2(c.seed, (uint32_t)i);
    int li = (len * (150 + (int)(hh & 105))) >> 8;
    li = (li * pump) >> 8;
    int ca = cos16(a), sa = sin16(a);
    int x0 = c.cx + ((ca * r0) >> 15), y0 = c.cy + ((sa * r0) >> 15);
    int x1 = c.cx + (int)(((int64_t)ca * li) >> 15), y1 = c.cy + (int)(((int64_t)sa * li) >> 15);
    line(c, x0, y0, x1, y1, c.w >= 480 ? 1 : 0, pal.c[(i * 256 / n) & 255], true);
  }
  int gr = c.unit / 10 + ((c.unit * (c.strobeOK ? c.onsetE : (c.onsetE >> 2))) >> 11);
  disc(c, c.cx, c.cy, gr, mulQ8(0xFFFFFF, 200), true);
  int flash = c.strobeOK ? (c.onset ? 256 : ((c.onsetE * c.onsetE) >> 9)) : 0;
  flash = (flash * c.ex[3]) >> 8;
  if (flash > 8) {
    uint32_t add = mulQ8(0xFFFFFF, (uint32_t)flash);
    uint32_t* p = c.px; for (int i = 0, m = c.w * c.h; i < m; ++i) p[i] = addSat(p[i], add);
  }
}

// ---- 10 scope_lissajous ----------------------------------------------------------------------------------------
void sScope(SceneCtx& c) {
  clearTo(c, c.bg);
  uint32_t dim = mulQ8(c.hue16 ? hsv(c.hue16, 120, 255) : 0x80C0FF, 40);
  hspan(c, c.cy, 0, c.w - 1, dim);
  for (int y = 0; y < c.h; ++y) c.px[(size_t)y * c.w + c.cx] = dim;
  int a = c.ex[0], b = c.ex[1];
  int tk = (c.ex[2] * c.w / 320) >> 9; if (tk < 0) tk = 0;
  int N = clampi(Q8i(c.ex[3]), 32, 1024);
  int amp = (c.unit * (190 + (c.rms >> 2) + (c.intens >> 3))) >> 8;
  uint32_t phi = phaseOf(c, 9000);
  Palette pal; pal.rainbow(c.hue16, 255);
  int px0 = 0, py0 = 0;
  for (int i = 0; i <= N; ++i) {
    uint32_t s = (uint32_t)(((int64_t)i << 16) / N);
    int x = c.cx + ((sin16((((uint32_t)a * s) >> 8) + phi) * amp) >> 15);
    int y = c.cy + ((sin16(((uint32_t)b * s) >> 8) * amp) >> 15);
    if (i) line(c, px0, py0, x, y, tk, pal.c[(i * 255 / N) & 255], true);
    px0 = x; py0 = y;
  }
}

// ---- 11 ripple_rings -------------------------------------------------------------------------------------------
void sRipple(SceneCtx& c) {
  clearTo(c, c.bg);
  int R = clampi(Q8i(c.ex[0]), 1, 12);
  int th0 = (c.unit * c.ex[1]) >> 8; if (th0 < 1) th0 = 1;
  int64_t P = ((int64_t)c.ex[2] * 1000000) >> 8; if (P < 200000) P = 200000;
  int spread = c.ex[3];
  Palette pal; pal.rainbow(c.hue16, 255);
  for (int k = 0; k < R; ++k) {
    int64_t tt = c.tUs + (int64_t)k * P / R;
    uint32_t cyc = (uint32_t)(tt / P);
    int fr = (int)((tt % P) * 256 / P);
    uint32_t hh = hash2(c.seed + (uint32_t)k * 31u, cyc);
    int ox = ((((int)(hh & 0xFFFF) - 32768) * (c.w / 2)) >> 15) * spread >> 8;
    int oy = ((((int)(hh >> 16) - 32768) * (c.h / 2)) >> 15) * spread >> 8;
    int radius = (int)(((int64_t)c.unit * 333 * fr) >> 16);
    int th = th0 + ((th0 * c.low) >> 9) + (fr >> 6);
    uint32_t col = mulQ8(pal.c[((hh >> 16) + (uint32_t)k * 40u) & 255], (uint32_t)(256 - fr) * (uint32_t)(150 + (c.intens >> 1)) >> 8);
    ring(c, c.cx + ox, c.cy + oy, radius, th, col);
  }
}

const SceneFn kFns[kSceneCount] = {sBars, sRibbon, sOrbit, sRadial, sParticles, sGrid, sTunnel, sColor, sPlasma, sStar, sScope, sRipple};

} // namespace

SceneFn sceneFn(int i) { return kFns[i < 0 ? 0 : (i >= kSceneCount ? kSceneCount - 1 : i)]; }
bool sceneNeedsPolar(int i) { return i == 3 || i == 6; }

void buildPolar(int w, int h, uint16_t* rad, uint16_t* ang, uint16_t* depth) {
  int mn = w < h ? w : h;
  int hw = (w + 1) / 2, hh = (h + 1) / 2;
  for (int y = 0; y < hh; ++y) {
    int dy = y * 2 + 1 - h;                      // half-pixel units, negative in the top half
    for (int x = 0; x < hw; ++x) {
      int dx = x * 2 + 1 - w;
      uint32_t dist = isqrt32((uint32_t)(dx * dx + dy * dy));
      uint32_t r = dist * 256u / (uint32_t)mn; if (r > 65535u) r = 65535u;
      uint32_t d = 8388608u / (r + 64u); if (d > 65535u) d = 65535u;
      uint16_t a = atan2Turns(dy, dx);
      int xs[2] = {x, w - 1 - x}, ys[2] = {y, h - 1 - y};
      uint16_t as[4] = {a, (uint16_t)((32768 - a) & 0xFFFF), (uint16_t)((65536 - a) & 0xFFFF), (uint16_t)((a + 32768) & 0xFFFF)};
      for (int q = 0; q < 4; ++q) {
        size_t i = (size_t)ys[q >> 1] * w + xs[q & 1];
        rad[i] = (uint16_t)r; depth[i] = (uint16_t)d; ang[i] = as[q];
      }
    }
  }
}

}} // namespace
