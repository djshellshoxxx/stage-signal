#include "render/renderer.h"
#include "render/scene_ctx.h"
#include "render/text.h"
#include <cstring>
#include <algorithm>

namespace ss {
using namespace rfx;

namespace {

int q8(float v) { double d = (double)v * 256.0 + 0.5; int i = (int)d; if (d < 0 && (double)i != d) --i; return i; }
int q16(float v) { double d = (double)v * 65536.0 + 0.5; int i = (int)d; if (d < 0 && (double)i != d) --i; return i; }
int clampQ8(float v) { return clampi(q8(v), 0, 256); }

struct Polar {
  std::vector<uint16_t> rad, ang, dep;
  int w = 0, h = 0;
  void reserve(size_t n) { rad.reserve(n); ang.reserve(n); dep.reserve(n); }
  void ensure(int W, int H) {
    if (w == W && h == H && !rad.empty()) return;
    size_t n = (size_t)W * H;
    rad.resize(n); ang.resize(n); dep.resize(n);
    buildPolar(W, H, rad.data(), ang.data(), dep.data());
    w = W; h = H;
  }
  size_t cap() const { return (rad.capacity() + ang.capacity() + dep.capacity()) * 2; }
};

struct Feat { int low, mid, high, onsetE, rms; bool onset, silent; };

Feat makeFeat(const FeatureSnapshot& f) {
  Feat o;
  o.low = clampQ8(f.band[0]); o.mid = clampQ8(f.band[1]); o.high = clampQ8(f.band[2]);
  o.onsetE = clampQ8(f.onsetStrength);
  double db = (double)f.rmsDbMix;
  double v = (db + 60.0) * 256.0 / 60.0;
  o.rms = v <= 0 ? 0 : (v >= 256 ? 256 : (int)v);
  o.onset = f.onset; o.silent = f.silent;
  return o;
}

struct SrcMap { int dx, dy, dw, dh, sx, sy, sw, sh; };   // dest sub-rect and source region (integers)

// Compute aspect fit/crop/stretch of a source region into a destination rect.
SrcMap computeFit(int srcW, int srcH, const NormRect& crop, int dx, int dy, int dw, int dh, FitMode mode) {
  SrcMap m{};
  int cx0 = clampi(q16(crop.x), 0, 65535), cy0 = clampi(q16(crop.y), 0, 65535);
  int cw = clampi(q16(crop.w), 1, 65536), ch = clampi(q16(crop.h), 1, 65536);
  m.sx = (int)(((int64_t)srcW * cx0) >> 16); m.sy = (int)(((int64_t)srcH * cy0) >> 16);
  m.sw = (int)(((int64_t)srcW * cw) >> 16); m.sh = (int)(((int64_t)srcH * ch) >> 16);
  if (m.sw < 1) m.sw = 1;
  if (m.sh < 1) m.sh = 1;
  if (m.sx + m.sw > srcW) m.sw = srcW - m.sx;
  if (m.sy + m.sh > srcH) m.sh = srcH - m.sy;
  if (m.sw < 1) { m.sw = 1; m.sx = srcW - 1; }
  if (m.sh < 1) { m.sh = 1; m.sy = srcH - 1; }
  m.dx = dx; m.dy = dy; m.dw = dw; m.dh = dh;
  if (mode == FitMode::Fit) {
    if ((int64_t)m.sw * dh > (int64_t)m.sh * dw) { m.dh = (int)((int64_t)m.sh * dw / m.sw); if (m.dh < 1) m.dh = 1; }
    else { m.dw = (int)((int64_t)m.sw * dh / m.sh); if (m.dw < 1) m.dw = 1; }
    m.dx = dx + (dw - m.dw) / 2; m.dy = dy + (dh - m.dh) / 2;
  } else if (mode == FitMode::Crop) {
    if ((int64_t)m.sw * dh > (int64_t)m.sh * dw) { int nw = (int)((int64_t)m.sh * dw / dh); if (nw < 1) nw = 1; m.sx += (m.sw - nw) / 2; m.sw = nw; }
    else { int nh = (int)((int64_t)m.sw * dh / dw); if (nh < 1) nh = 1; m.sy += (m.sh - nh) / 2; m.sh = nh; }
  }
  return m;
}

bool frameOk(const Frame* f) { return f && f->w > 0 && f->h > 0 && f->px.size() >= (size_t)f->w * (size_t)f->h; }

inline uint32_t gainPx(uint32_t c, int g) {   // g Q8 0..1024 with saturation when >256
  if (g <= 256) return mulQ8(c, (uint32_t)g);
  uint32_t r = (((c >> 16) & 255) * (uint32_t)g) >> 8, gg = (((c >> 8) & 255) * (uint32_t)g) >> 8, b = ((c & 255) * (uint32_t)g) >> 8;
  if (r > 255) r = 255;
  if (gg > 255) gg = 255;
  if (b > 255) b = 255;
  return (r << 16) | (gg << 8) | b;
}

} // namespace

struct Renderer::Impl {
  int outW = 0, outH = 0, pmMax = 1000;
  size_t capPx = 0;
  std::vector<uint32_t> bufA, bufB, acc;
  std::vector<int32_t> scratch;
  std::vector<int> xmap;
  Polar polarFull, polarHalf;
  TextEngine text;
  RenderStats st;
  // temporal state
  bool trailValid = false; int trailW = 0, trailH = 0; SrcMap trailRect{}; Ns trailLastNs = 0;
  int prevL = -1;                 // strobe guard: last luminance (0..255*16)
  Ns guardLastNs = 0;

  size_t poolBytes() const {
    return (bufA.capacity() + bufB.capacity() + acc.capacity()) * 4 + scratch.capacity() * 4 + xmap.capacity() * 4 + polarFull.cap() + polarHalf.cap();
  }
  void resetTemporal() { trailValid = false; prevL = -1; }

  // ---- scene rendering --------------------------------------------------------------------------------------
  int effectiveScene(int idx, const ShedKnobs& shed) const {
    idx = clampi(idx, 0, kSceneCount - 1);
    if (shed.fallbackScenes && sceneInfo(idx).cost != CostClass::Low) idx = sceneInfo(idx).fallback;
    return idx;
  }
  void renderScene(const SceneLayer& L, uint32_t* dst, int w, int h, const Feat& f, const RenderState& s, bool half) {
    int orig = clampi(L.scene, 0, kSceneCount - 1);
    int idx = effectiveScene(orig, s.shed);
    SceneParams p = idx == orig ? clampParams(orig, L.params) : clampParams(idx, [&] { SceneParams q = L.params; for (int i = 0; i < 4; ++i) q.extra[i] = kParamDefault; return q; }());
    SceneCtx c;
    c.px = dst; c.w = w; c.h = h; c.unit = std::min(w, h) / 2; if (c.unit < 1) c.unit = 1; c.cx = w / 2; c.cy = h / 2;
    int64_t tUs = (L.timeNs < 0 ? 0 : L.timeNs) / 1000;
    int64_t sp = (int64_t)q16(p.speed);
    c.tUs = (tUs * sp) >> 16;
    c.seed = L.seed; c.bg = s.background & 0xFFFFFF;
    c.intens = clampQ8(p.intensity);
    c.hue16 = q16(p.hue) & 0xFFFF;
    c.low = f.low; c.mid = f.mid; c.high = f.high; c.onsetE = f.onsetE; c.rms = f.rms; c.onset = f.onset; c.silent = f.silent;
    for (int i = 0; i < 4; ++i) c.ex[i] = q8(p.extra[i]);
    c.particlePct = clampi(s.shed.particlePercent, 5, 100);
    c.strobeOK = !s.disableStrobing;
    c.scratch = scratch.data(); c.scratchN = (int)scratch.size();
    if (sceneNeedsPolar(idx)) {
      Polar& P = half ? polarHalf : polarFull;
      P.ensure(w, h);
      c.rad = P.rad.data(); c.ang = P.ang.data(); c.depth = P.dep.data();
    }
    sceneFn(idx)(c);
    int bq = clampQ8(p.brightness);
    if (bq < 256) { size_t n = (size_t)w * h; for (size_t i = 0; i < n; ++i) dst[i] = mulQ8(dst[i], (uint32_t)bq); }
  }
  bool sceneIsStrobe(const SceneLayer& L, const ShedKnobs& shed) const { return sceneInfo(effectiveScene(L.scene, shed)).strobe; }

  // ---- layers -----------------------------------------------------------------------------------------------
  void blitMedia(uint32_t* dst, int W, int H, const MediaLayer& m) {
    const Frame& f = *m.frame;
    int op = clampQ8(m.opacity);
    if (op <= 0) return;
    SrcMap sm = computeFit(f.w, f.h, m.crop, 0, 0, W, H, m.fit);
    buildXmap(sm, false);
    for (int y = 0; y < sm.dh; ++y) {
      int sy = sm.sy + (int)(((int64_t)y * sm.sh + sm.sh / 2) / sm.dh);
      const uint32_t* srow = f.px.data() + (size_t)sy * f.w;
      uint32_t* d = dst + (size_t)(sm.dy + y) * W + sm.dx;
      if (op >= 256) for (int x = 0; x < sm.dw; ++x) d[x] = srow[xmap[x]] & 0xFFFFFF;
      else for (int x = 0; x < sm.dw; ++x) d[x] = lerpQ8(d[x], srow[xmap[x]] & 0xFFFFFF, (uint32_t)op);
    }
  }
  void buildXmap(const SrcMap& sm, bool mirror) {
    if ((int)xmap.size() < sm.dw) xmap.resize(sm.dw);     // within reserved capacity
    for (int x = 0; x < sm.dw; ++x) {
      int sx = (int)(((int64_t)x * sm.sw + sm.sw / 2) / sm.dw);
      xmap[x] = mirror ? sm.sx + sm.sw - 1 - sx : sm.sx + sx;
    }
  }

  // Camera layer: PIP / split / full, with mirror, threshold, posterize, tint, trails. Returns whether trails were active.
  bool blitCamera(uint32_t* dst, int W, int H, const CameraLayer& cam, const RenderState& s) {
    const CameraTreatments& t = cam.t;
    const Frame& f = *cam.frame;
    int dx = 0, dy = 0, dw = W, dh = H;
    if (t.pip) {
      dx = clampi(q16(t.pipRect.x) * W >> 16, 0, W - 2); dy = clampi(q16(t.pipRect.y) * H >> 16, 0, H - 2);
      dw = clampi(q16(t.pipRect.w) * W >> 16, 2, W - dx); dh = clampi(q16(t.pipRect.h) * H >> 16, 2, H - dy);
    } else if (t.split) {
      int half = W / 2;
      if (t.splitCameraRight) { dx = half; dw = W - half; } else { dx = 0; dw = half; }
    }
    SrcMap sm = computeFit(f.w, f.h, t.crop, dx, dy, dw, dh, t.fit);
    buildXmap(sm, t.mirror);
    // channel LUTs: posterize then tint; threshold handled before LUT via luma
    uint8_t lr[256], lg[256], lb[256];
    int levels = clampi(t.posterizeLevels, 2, 16);
    int amt = clampQ8(t.tintAmount);
    int tr = (t.tintRgb >> 16) & 255, tg = (t.tintRgb >> 8) & 255, tb = t.tintRgb & 255;
    for (int c = 0; c < 256; ++c) {
      int v = c;
      if (t.posterize) { int qv = (c * levels) >> 8; v = (qv * 255) / (levels - 1); }
      lr[c] = (uint8_t)(v + (((tr - v) * amt) / 256));
      lg[c] = (uint8_t)(v + (((tg - v) * amt) / 256));
      lb[c] = (uint8_t)(v + (((tb - v) * amt) / 256));
    }
    int cut = clampi(q8(t.thresholdCutoff) * 255 >> 8, 0, 255);
    bool thr = t.threshold;
    // trails
    int N = std::min(t.trailFrames, s.shed.trailFramesCap);
    bool trail = N >= 2 && !(sm.dw < 1);
    int decay = 256;
    if (trail) {
      bool same = trailValid && trailW == W && trailH == H && trailRect.dx == sm.dx && trailRect.dy == sm.dy && trailRect.dw == sm.dw && trailRect.dh == sm.dh;
      int dtF = 1;
      if (same) {
        Ns dt = s.elapsedNs - trailLastNs;
        dtF = dt < 0 ? 1 : (int)clampi((int)((dt * 25 + 500000000LL) / 1000000000LL), 0, 8);
      }
      int k1 = 256 - 256 / N, k = 256;
      for (int i = 0; i < dtF; ++i) k = (k * k1) >> 8;
      decay = same ? k : -1;           // -1 = first frame: no history
      trailValid = true; trailW = W; trailH = H; trailRect = sm; trailLastNs = s.elapsedNs;
    } else trailValid = false;

    for (int y = 0; y < sm.dh; ++y) {
      int sy = sm.sy + (int)(((int64_t)y * sm.sh + sm.sh / 2) / sm.dh);
      const uint32_t* srow = f.px.data() + (size_t)sy * f.w;
      uint32_t* d = dst + (size_t)(sm.dy + y) * W + sm.dx;
      uint32_t* a = trail ? acc.data() + (size_t)(sm.dy + y) * W + sm.dx : nullptr;
      for (int x = 0; x < sm.dw; ++x) {
        uint32_t p = srow[xmap[x]];
        int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        if (thr) { int l = (r * 77 + g * 150 + b * 29) >> 8; r = g = b = l >= cut ? 255 : 0; }
        uint32_t o = ((uint32_t)lr[r] << 16) | ((uint32_t)lg[g] << 8) | lb[b];
        if (a) {
          if (decay >= 0) o = maxRgb(o, mulQ8(a[x], (uint32_t)decay));
          a[x] = o;
        }
        d[x] = o;
      }
    }
    return trail;
  }

  // ---- text -------------------------------------------------------------------------------------------------
  struct TextPlace { int px, maxW; };
  void drawText(Frame& out, const IRect& fit, const TextOverlay& t, int masterQ) {
    if (t.text.empty()) return;
    int a = clampQ8(t.alpha);
    if (a <= 0) return;
    int px = clampi(q16(t.sizeFrac) * fit.h >> 16, 12, std::max(12, fit.h));
    int bx = fit.x + (q16(t.box.x) * fit.w >> 16), by = fit.y + (q16(t.box.y) * fit.h >> 16);
    int bw = clampi(q16(t.box.w) * fit.w >> 16, 1, fit.w), bh = clampi(q16(t.box.h) * fit.h >> 16, 1, fit.h);
    const TextBitmap* bm = text.get(t.text, px, bw, t.h, t.bold);
    if (!bm || bm->w <= 0 || bm->h <= 0) return;
    int ox = t.h == HAlign::Left ? 0 : (t.h == HAlign::Center ? (bw - bm->w) / 2 : bw - bm->w);
    if (ox < 0) ox = 0;
    int sy = 0, ch = bm->h, oy = 0;
    if (bm->h <= bh) oy = t.v == VAlign::Top ? 0 : (t.v == VAlign::Middle ? (bh - bm->h) / 2 : bh - bm->h);
    else { sy = clampi(t.scrollPx, 0, bm->h - bh); ch = bh; }
    int cw = std::min(bm->w, bw);
    if (t.shadow) {
      int d = std::max(1, px / 24);
      // shadow clipped to the same box by shrinking the source window
      blitMask(out.px.data(), out.w, out.h, *bm, 0, sy, std::max(0, cw - d), std::max(0, ch - d), bx + ox + d, by + oy + d, 0u, (a * 160) >> 8);
    }
    blitMask(out.px.data(), out.w, out.h, *bm, 0, sy, cw, ch, bx + ox, by + oy, mulQ8(t.color & 0xFFFFFF, (uint32_t)masterQ), a);
  }
};

Renderer::Renderer() : p_(new Impl) {}
Renderer::~Renderer() {}

IRect Renderer::fitRect(int outW, int outH, int an, int ad) {
  IRect r{0, 0, outW, outH};
  if (an <= 0 || ad <= 0 || outW <= 0 || outH <= 0) return r;
  if ((int64_t)outW * ad > (int64_t)outH * an) { r.w = (int)((int64_t)outH * an / ad); r.h = outH; }
  else { r.w = outW; r.h = (int)((int64_t)outW * ad / an); }
  if (r.w < 1) r.w = 1;
  if (r.h < 1) r.h = 1;
  r.x = (outW - r.w) / 2; r.y = (outH - r.h) / 2;
  return r;
}

static void internalDims(const IRect& fit, int pm, int& iw, int& ih) {
  iw = std::max(16, (int)((int64_t)fit.w * pm / 1000));
  ih = std::max(16, (int)((int64_t)fit.h * pm / 1000));
}

Status Renderer::configure(int outW, int outH, float renderScale) {
  Impl& p = *p_;
  if (outW < 16 || outH < 16 || outW > 4096 || outH > 4096) return Status::Fail(Err::InvalidArgument, "output size out of range");
  int pm = clampi(q16(renderScale) * 1000 >> 16, 100, 1000);
  int iw, ih; internalDims(IRect{0, 0, outW, outH}, pm, iw, ih);
  size_t cap = (size_t)iw * ih;
  size_t half = ((size_t)iw / 2 + 1) * ((size_t)ih / 2 + 1);
  size_t total = cap * 4 * 3 + cap * 6 + half * 6 + (size_t)(outW * 3 + outH + 64) * 4;
  if (total > Renderer::kPoolBudgetBytes) return Status::Fail(Err::ResourceExhausted, "render pools exceed 12 MiB");
  p.outW = outW; p.outH = outH; p.pmMax = pm; p.capPx = cap;
  p.bufA.assign(cap, 0); p.bufB.assign(cap, 0); p.acc.assign(cap, 0);
  p.scratch.assign((size_t)outW * 3 + outH + 64, 0);
  p.xmap.assign((size_t)std::max(outW, outH) + 16, 0);
  p.polarFull = Polar(); p.polarHalf = Polar();
  p.polarFull.reserve(cap); p.polarHalf.reserve(half);
  p.polarFull.ensure(iw, ih);                          // prebuild for the default composition
  p.polarHalf.ensure(std::max(8, iw / 2), std::max(8, ih / 2));
  p.text.clear();
  p.resetTemporal();
  p.st = RenderStats();
  return Status::Ok();
}

void Renderer::setTextRasterizer(ITextRasterizer* r) { p_->text.setRasterizer(r); }
size_t Renderer::poolBytes() const { return p_->poolBytes(); }
size_t Renderer::textCacheBytes() const { return p_->text.bytes(); }
const RenderStats& Renderer::stats() const { return p_->st; }

void Renderer::prepareText(const TextOverlay& t, int an, int ad) {
  Impl& p = *p_;
  if (!p.outW || t.text.empty()) return;
  IRect fit = fitRect(p.outW, p.outH, an, ad);
  int px = clampi(q16(t.sizeFrac) * fit.h >> 16, 12, std::max(12, fit.h));
  int bw = clampi(q16(t.box.w) * fit.w >> 16, 1, fit.w);
  p.text.get(t.text, px, bw, t.h, t.bold);
}

static const char* const kPc[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void appendAscii(std::u32string& s, const char* a) { while (*a) s.push_back((char32_t)(unsigned char)*a++); }
static void appendInt(std::u32string& s, int v) { char b[16]; int n = 0; if (v < 0) { s.push_back('-'); v = -v; } do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) s.push_back((char32_t)b[--n]); }
static void appendEst(std::u32string& s, const Estimate& e) {
  appendAscii(s, e.source == Estimate::Manual ? " MAN" : (e.source == Estimate::Tap ? " TAP" : " EST"));
  if (e.source == Estimate::Estimated) {
    int c = clampi((int)(e.confidence * 100.0f + 0.5f), 0, 100);
    s.push_back(' '); appendInt(s, c); s.push_back('%');
    if (e.ageNs > 10 * kNsPerSec) appendAscii(s, " OLD");
  }
}

void Renderer::render(Frame& out, const RenderState& s) {
  Impl& p = *p_;
  if (p.outW == 0) return;
  const size_t outN = (size_t)p.outW * p.outH;
  if (out.w != p.outW || out.h != p.outH || out.px.size() != outN) out.resize(p.outW, p.outH);
  out.pts = s.elapsedNs; out.generation = s.generation;
  p.st.halfResTransition = false; p.st.strobeGuardActive = false; p.st.trailsActive = false; p.st.sceneA = p.st.sceneB = -1;

  const int masterQ = clampQ8(s.masterBrightness);
  if (s.blackout || masterQ == 0) {                      // latched blackout: black on this very call, no other work
    std::memset(out.px.data(), 0, outN * sizeof(uint32_t));
    p.resetTemporal();
    return;
  }

  IRect fit = fitRect(p.outW, p.outH, s.compAspectNum, s.compAspectDen);
  p.st.fit = fit;
  int pm = p.pmMax;
  if (s.internalScale > 0.0f) pm = clampi(q16(s.internalScale) * 1000 >> 16, 100, p.pmMax);
  int iw, ih; internalDims(fit, pm, iw, ih);
  if ((size_t)iw * ih > p.capPx) { iw = std::min(iw, p.outW); ih = std::min(ih, p.outH); if ((size_t)iw * ih > p.capPx) { iw = p.outW; ih = (int)(p.capPx / (size_t)iw); } }
  p.st.internalW = iw; p.st.internalH = ih;
  const Feat f = makeFeat(s.features);
  uint32_t* comp = p.bufA.data();

  // ---- background -> procedural/media ---------------------------------------------------------------------
  bool mediaOk = frameOk(s.media.frame) && s.media.opacity > 0.0f;
  bool mediaCovers = false;
  if (mediaOk && clampQ8(s.media.opacity) >= 256 && s.media.fit != FitMode::Fit) mediaCovers = true;
  bool guard = false;
  if (mediaCovers) {
    std::memset(comp, 0, (size_t)iw * ih * 4);
  } else {
    TransitionType tt = s.transition;
    int q = clampQ8(s.transitionProgress);
    const SceneLayer* only = nullptr;
    if (tt == TransitionType::None || (tt == TransitionType::Crossfade && q <= 0)) only = &s.a;
    else if (tt == TransitionType::Cut || q >= 256) only = &s.b;
    if (only) {
      p.renderScene(*only, comp, iw, ih, f, s, false);
      (only == &s.a ? p.st.sceneA : p.st.sceneB) = p.effectiveScene(only->scene, s.shed);
      guard = s.disableStrobing && p.sceneIsStrobe(*only, s.shed);
    } else {
      p.st.sceneA = p.effectiveScene(s.a.scene, s.shed); p.st.sceneB = p.effectiveScene(s.b.scene, s.shed);
      guard = s.disableStrobing && (p.sceneIsStrobe(s.a, s.shed) || p.sceneIsStrobe(s.b, s.shed));
      if (s.shed.halfResTransition && iw >= 32 && ih >= 32) {
        int hw = iw / 2, hh = ih / 2;
        uint32_t* ha = p.bufB.data(); uint32_t* hb = ha + (size_t)hw * hh;
        p.renderScene(s.a, ha, hw, hh, f, s, true);
        p.renderScene(s.b, hb, hw, hh, f, s, true);
        for (int y = 0; y < ih; ++y) {
          int sy = std::min(y >> 1, hh - 1);
          const uint32_t* ra = ha + (size_t)sy * hw; const uint32_t* rb = hb + (size_t)sy * hw;
          uint32_t* d = comp + (size_t)y * iw;
          for (int x = 0; x < iw; ++x) { int sx = std::min(x >> 1, hw - 1); d[x] = lerpQ8(ra[sx], rb[sx], (uint32_t)q); }
        }
        p.st.halfResTransition = true;
      } else {
        uint32_t* B = p.bufB.data();
        p.renderScene(s.a, comp, iw, ih, f, s, false);
        p.renderScene(s.b, B, iw, ih, f, s, false);
        size_t n = (size_t)iw * ih;
        for (size_t i = 0; i < n; ++i) comp[i] = lerpQ8(comp[i], B[i], (uint32_t)q);
      }
    }
  }
  // ---- strobe slew guard (only when strobing is disabled and a strobe-flagged scene is on screen) -------------
  if (guard) {
    size_t n = (size_t)iw * ih; int64_t sum = 0; int cnt = 0;
    for (size_t i = 0; i < n; i += 7) { sum += luma8(comp[i]); ++cnt; }
    int L = cnt ? (int)(sum * 16 / cnt) : 0;           // 0..4080
    int lim = 16 * 16;                                  // ~6% of full range per frame (x16 fixed point)
    if (p.prevL >= 0) {
      Ns dt = s.elapsedNs - p.guardLastNs;
      int dtF = dt < 0 ? 1 : clampi((int)((dt * 25 + 500000000LL) / 1000000000LL), 1, 4);
      lim *= dtF;
    }
    int Lt = L;
    if (p.prevL >= 0) Lt = clampi(L, p.prevL - lim, p.prevL + lim);
    if (Lt != L && L > 0) {
      int g = clampi((int)((int64_t)Lt * 256 / L), 0, 1024);
      for (size_t i = 0; i < n; ++i) comp[i] = gainPx(comp[i], g);
    }
    p.prevL = Lt; p.guardLastNs = s.elapsedNs; p.st.strobeGuardActive = true;
  } else p.prevL = -1;
  if (mediaOk) p.blitMedia(comp, iw, ih, s.media);
  // ---- camera --------------------------------------------------------------------------------------------------
  if (s.camera.routed && frameOk(s.camera.frame)) p.st.trailsActive = p.blitCamera(comp, iw, ih, s.camera, s);
  else p.trailValid = false;
  // ---- master brightness (text/overlays fold it into their colors: linear, so equivalent to a final multiply) ----
  if (masterQ < 256) { size_t n = (size_t)iw * ih; for (size_t i = 0; i < n; ++i) comp[i] = mulQ8(comp[i], (uint32_t)masterQ); }

  // ---- aspect-fit scale to output (letterbox/pillarbox black) ------------------------------------------------
  uint32_t* o = out.px.data();
  if (fit.w != p.outW || fit.h != p.outH) {
    for (int y = 0; y < p.outH; ++y) {
      uint32_t* row = o + (size_t)y * p.outW;
      if (y < fit.y || y >= fit.y + fit.h) std::memset(row, 0, (size_t)p.outW * 4);
      else { if (fit.x > 0) std::memset(row, 0, (size_t)fit.x * 4); int rx = fit.x + fit.w; if (rx < p.outW) std::memset(row + rx, 0, (size_t)(p.outW - rx) * 4); }
    }
  }
  if ((int)p.xmap.size() < fit.w) p.xmap.resize(fit.w);
  for (int x = 0; x < fit.w; ++x) p.xmap[x] = (int)(((int64_t)(2 * x + 1) * iw) / (2 * (int64_t)fit.w));
  int prevSy = -1;
  for (int y = 0; y < fit.h; ++y) {
    int sy = (int)(((int64_t)(2 * y + 1) * ih) / (2 * (int64_t)fit.h));
    uint32_t* d = o + (size_t)(fit.y + y) * p.outW + fit.x;
    if (sy == prevSy) std::memcpy(d, d - p.outW, (size_t)fit.w * 4);
    else if (iw == fit.w) std::memcpy(d, comp + (size_t)sy * iw, (size_t)fit.w * 4);
    else { const uint32_t* srow = comp + (size_t)sy * iw; const int* xm = p.xmap.data(); for (int x = 0; x < fit.w; ++x) d[x] = srow[xm[x]]; }
    prevSy = sy;
  }

  // ---- text -> overlays (output resolution) ----------------------------------------------------------------------
  size_t nt = std::min(s.texts.size(), kMaxTextOverlays);
  for (size_t i = 0; i < nt; ++i) p.drawText(out, fit, s.texts[i], masterQ);
  const OverlayOptions& ov = s.overlays;
  int ovA = clampQ8(ov.alpha);
  if (ovA > 0 && (ov.showTempo || ov.showKey)) {
    std::u32string t;
    if (ov.showTempo) {
      appendAscii(t, "BPM ");
      if (s.features.tempo.valid) { appendInt(t, (int)(s.features.tempo.value + 0.5)); appendEst(t, s.features.tempo); } else appendAscii(t, "--");
    }
    if (ov.showKey) {
      if (!t.empty()) t.push_back('\n');
      appendAscii(t, "KEY ");
      if (s.features.key.valid) {
        appendAscii(t, kPc[clampi((int)(s.features.key.value + 0.5), 0, 11)]);
        appendAscii(t, s.features.key.minor ? " MIN" : " MAJ"); appendEst(t, s.features.key);
      } else appendAscii(t, "--");
    }
    TextOverlay to; to.text = t; to.box = NormRect{0.02f, 0.02f, 0.5f, 0.2f}; to.h = HAlign::Left; to.v = VAlign::Top;
    to.sizeFrac = 0.045f; to.alpha = ov.alpha; to.color = 0xFFFFFF; to.shadow = true;
    p.drawText(out, fit, to, masterQ);
  }
  if (ovA > 0 && !ov.brandingText.empty()) {
    TextOverlay to; to.text = ov.brandingText; to.box = NormRect{0.02f, 0.93f, 0.5f, 0.06f}; to.h = HAlign::Left; to.v = VAlign::Bottom;
    to.sizeFrac = 0.04f; to.alpha = ov.alpha; to.color = 0xFFFFFF;
    p.drawText(out, fit, to, masterQ);
  }
  if (ovA > 0 && frameOk(ov.brandingImage)) {
    const Frame& b = *ov.brandingImage;
    int bh = clampi(q16(ov.brandingHeightFrac) * fit.h >> 16, 4, fit.h / 2);
    int bw = std::max(1, (int)((int64_t)b.w * bh / b.h)); if (bw > fit.w / 2) { bw = fit.w / 2; bh = std::max(1, (int)((int64_t)b.h * bw / b.w)); }
    int mx = fit.w / 50, my = fit.h / 50;
    int x0 = fit.x + fit.w - bw - mx, y0 = fit.y + fit.h - bh - my;
    for (int y = 0; y < bh; ++y) {
      int oy = y0 + y; if (oy < 0 || oy >= p.outH) continue;
      const uint32_t* srow = b.px.data() + (size_t)((int64_t)y * b.h / bh) * b.w;
      for (int x = 0; x < bw; ++x) {
        int ox = x0 + x; if (ox < 0 || ox >= p.outW) continue;
        uint32_t c = mulQ8(srow[(int64_t)x * b.w / bw] & 0xFFFFFF, (uint32_t)masterQ);
        uint32_t& d = o[(size_t)oy * p.outW + ox];
        d = ovA >= 256 ? c : lerpQ8(d, c, (uint32_t)ovA);
      }
    }
  }
  // ---- reference border / corner markers (REQ-D04) --------------------------------------------------------------
  if (s.refBorder) {
    int t = std::max(2, fit.h / 150);
    auto box = [&](int x0, int y0, int w, int h, uint32_t c) {
      for (int y = std::max(0, y0); y < std::min(p.outH, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min(p.outW, x0 + w); ++x) o[(size_t)y * p.outW + x] = c;
    };
    box(fit.x, fit.y, fit.w, t, 0xFFFFFF); box(fit.x, fit.y + fit.h - t, fit.w, t, 0xFFFFFF);
    box(fit.x, fit.y, t, fit.h, 0xFFFFFF); box(fit.x + fit.w - t, fit.y, t, fit.h, 0xFFFFFF);
    int c = t * 4;
    box(fit.x, fit.y, c, c, 0xFF0000); box(fit.x + fit.w - c, fit.y, c, c, 0x00FF00);
    box(fit.x, fit.y + fit.h - c, c, c, 0x0000FF); box(fit.x + fit.w - c, fit.y + fit.h - c, c, c, 0xFFFF00);
  }
}

} // namespace ss
