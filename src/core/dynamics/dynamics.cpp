#include "core/dynamics/dynamics.h"
#include "core/analysis/dsp.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ss {

namespace {
constexpr float kLn10Over20 = 0.115129254649702f;           // ln(10)/20
inline float clampf(float v, float lo, float hi, float def) {
  if (!dsp::isFinite(v)) return def;
  return v < lo ? lo : (v > hi ? hi : v);
}
inline float dbToLin(float db) { return std::exp(db * kLn10Over20); }

// Monotone-deque sliding extremum over the last `win` samples; capacity fixed at construction.
struct SlidingExt {
  std::vector<float> v; std::vector<uint64_t> idx;
  size_t head = 0, count = 0, cap = 0; uint64_t n = 0; uint32_t win = 1; bool isMax = true;
  void init(uint32_t w, bool mx, float fill) {
    win = w; isMax = mx; cap = (size_t)w + 2; v.assign(cap, fill); idx.assign(cap, 0); head = 0; count = 0; n = 0;
  }
  inline float push(float x) {
    while (count) {
      const size_t b = (head + count - 1) % cap;
      if (isMax ? (v[b] <= x) : (v[b] >= x)) --count; else break;
    }
    const size_t t = (head + count) % cap;
    v[t] = x; idx[t] = n; ++count;
    while (idx[head] + win <= n) { head = (head + 1) % cap; --count; }
    ++n;
    return v[head];
  }
};
} // namespace

DynamicsConfig DynamicsConfig::clamped() const {
  DynamicsConfig d;
  d.bypass = bypass; d.compressorOn = compressorOn; d.limiterOn = limiterOn;
  d.trimDb = clampf(trimDb, -24.f, 12.f, 0.f);
  d.thresholdDb = clampf(thresholdDb, -36.f, 0.f, -12.f);
  d.ratio = clampf(ratio, 1.f, 10.f, 3.f);
  d.attackMs = clampf(attackMs, 0.5f, 100.f, 10.f);
  d.releaseMs = clampf(releaseMs, 20.f, 2000.f, 150.f);
  d.kneeDb = clampf(kneeDb, 0.f, 12.f, 6.f);
  d.makeupDb = clampf(makeupDb, 0.f, 12.f, 0.f);
  d.ceilingDb = clampf(ceilingDb, -12.f, -0.5f, -1.f);
  d.limiterReleaseMs = clampf(limiterReleaseMs, 20.f, 500.f, 100.f);
  return d;
}

struct Dynamics::Impl {
  uint32_t rate, ch, D;
  // ---- config hand-off (lock-free triple buffer; one staging thread)
  DynamicsConfig slots[3]; uint64_t slotRev[3] = {0, 0, 0};
  std::atomic<int> mid{1}; int front = 0; int back = 2;      // bit 2 of mid = "new data"
  std::atomic<uint64_t> revCounter{0};
  DynamicsConfig cfg; uint64_t cfgRev = 0;

  // ---- derived parameters
  float trimTgt = 1.f, trimCur = 1.f, trimStep = 1.f;
  float mkTgt = 1.f, mkCur = 1.f, mkStep = 1.f;
  float thr = -12.f, slope = 2.f / 3.f, knee = 6.f, atk = 0.f, rel = 0.f;
  float ceilLin = 0.891f, limRel = 0.f;
  bool compOn = true, limOn = true;
  float bTgt = 0.f, bCur = 0.f, bStep = 0.f;
  float compLoLin = 0.f;

  // ---- state
  std::vector<float> dryBuf, wetBuf; uint32_t dpos = 0;
  SlidingExt detMax, limMin;
  std::vector<float> boxRing; uint32_t bpos = 0; double boxSum = 0;
  float gr = 0.f;                    // compressor smoothed gain reduction (dB, >=0)
  float G = 1.f;                     // limiter gain
  // ---- telemetry
  float inPk = 0.f, outPk = 0.f;
  float grNow = 0.f, limNow = 0.f, grMax = 0.f, limMaxDb = 0.f;
  uint64_t frames = 0, compAct = 0, limAct = 0, byAct = 0, sanitized = 0;

  Impl(uint32_t r, uint32_t c) : rate(r), ch(c) {
    D = std::max<uint32_t>(1, (uint32_t)(0.005 * r + 0.5));
    dryBuf.assign((size_t)D * ch, 0.f); wetBuf.assign((size_t)D * ch, 0.f);
    detMax.init(D, true, 0.f); limMin.init(D + 1, false, 1.f);
    boxRing.assign(D + 1, 1.f);
    for (int i = 0; i < 3; ++i) slots[i] = DynamicsConfig();
    cfg = DynamicsConfig();
    apply(cfg, true);
    resetState();
  }

  void resetState() {
    std::fill(dryBuf.begin(), dryBuf.end(), 0.f); std::fill(wetBuf.begin(), wetBuf.end(), 0.f); dpos = 0;
    detMax.init(D, true, 0.f); limMin.init(D + 1, false, 1.f);
    std::fill(boxRing.begin(), boxRing.end(), 1.f); bpos = 0; boxSum = D + 1;
    gr = 0.f; G = 1.f;
    inPk = outPk = 0.f; grNow = limNow = grMax = limMaxDb = 0.f;
    frames = compAct = limAct = byAct = sanitized = 0;
    trimCur = trimTgt; mkCur = mkTgt; bCur = bTgt;
  }

  static float coef(float ms, uint32_t rate) { return std::exp(-1.0f / (ms * 0.001f * (float)rate)); }

  void apply(const DynamicsConfig& c, bool snap) {
    trimTgt = dbToLin(c.trimDb);
    compOn = c.compressorOn; limOn = c.limiterOn;
    mkTgt = compOn ? dbToLin(c.makeupDb) : 1.f;
    const float ramp = std::max(1.f, 0.010f * (float)rate);
    trimStep = std::fabs(trimTgt - trimCur) / ramp + 1e-9f;
    mkStep = std::fabs(mkTgt - mkCur) / ramp + 1e-9f;
    thr = c.thresholdDb; slope = 1.f - 1.f / c.ratio; knee = c.kneeDb;
    atk = coef(c.attackMs, rate); rel = coef(c.releaseMs, rate);
    ceilLin = dbToLin(c.ceilingDb); limRel = coef(c.limiterReleaseMs, rate);
    compLoLin = dbToLin(thr - 0.5f * knee);
    bTgt = c.bypass ? 1.f : 0.f; bStep = 1.f / ramp;
    if (snap) { trimCur = trimTgt; mkCur = mkTgt; bCur = bTgt; }
  }

  inline float gainComputer(float L) const {              // static curve: gain reduction in dB (>= 0)
    const float over = L - thr;
    if (knee <= 0.f) return over > 0.f ? slope * over : 0.f;
    if (2.f * over < -knee) return 0.f;
    if (2.f * std::fabs(over) <= knee) { const float t = over + 0.5f * knee; return slope * t * t / (2.f * knee); }
    return slope * over;
  }

  void pollConfig() {
    if (!(mid.load() & 4)) return;
    front = mid.exchange(front) & 3;
    cfg = slots[front]; cfgRev = slotRev[front];
    apply(cfg, false);
  }

  void run(const float* in, float* out, size_t frames_) {
    pollConfig();
    const uint32_t C = ch;
    float grMaxBlk = grNow, minG = 1.f;
    float x[16], cv[16];
    const float limAct99 = 0.98863f;                       // -0.1 dB
    for (size_t f = 0; f < frames_; ++f) {
      float inMax = 0.f;
      for (uint32_t c = 0; c < C; ++c) {
        float s = dsp::sanitize(in[f * C + c], sanitized);
        if (s > 1e4f) s = 1e4f; else if (s < -1e4f) s = -1e4f;
        x[c] = s; const float a = s < 0 ? -s : s; if (a > inMax) inMax = a;
      }
      if (inMax > inPk) inPk = inMax;
      // ramps
      if (trimCur != trimTgt) { const float d = trimTgt - trimCur; trimCur = std::fabs(d) <= trimStep ? trimTgt : trimCur + (d > 0 ? trimStep : -trimStep); }
      if (mkCur != mkTgt) { const float d = mkTgt - mkCur; mkCur = std::fabs(d) <= mkStep ? mkTgt : mkCur + (d > 0 ? mkStep : -mkStep); }
      if (bCur != bTgt) { const float d = bTgt - bCur; bCur = std::fabs(d) <= bStep ? bTgt : bCur + (d > 0 ? bStep : -bStep); }
      // trim + compressor (stereo-linked: detector = max channel, shared gain)
      float m = inMax * trimCur;
      const float mx = detMax.push(m);
      float target = 0.f;
      if (compOn && mx > compLoLin) target = gainComputer(20.f * std::log10(mx));
      gr = (target > gr ? atk : rel) * gr + (1.f - (target > gr ? atk : rel)) * target;
      if (gr < 1e-5f && target == 0.f) gr = 0.f;
      const float g = (gr > 0.f ? std::exp(-gr * kLn10Over20) : 1.f) * mkCur * trimCur;
      float pk = 0.f;
      for (uint32_t c = 0; c < C; ++c) { cv[c] = x[c] * g; const float a = cv[c] < 0 ? -cv[c] : cv[c]; if (a > pk) pk = a; }
      if (gr > grMaxBlk) grMaxBlk = gr;
      if (gr > 0.1f) ++compAct;
      // limiter: required gain -> trailing min over D+1 -> box average over D+1 -> release smoothing
      const float req = (limOn && pk > ceilLin) ? ceilLin / pk : 1.f;
      const float mn = limMin.push(req);
      boxSum += (double)mn - (double)boxRing[bpos]; boxRing[bpos] = mn; bpos = (bpos + 1 > D) ? 0 : bpos + 1;
      float A = (float)(boxSum / (double)(D + 1)); if (A > 1.f) A = 1.f;
      G = (A <= G) ? A : A + (G - A) * limRel;
      if (G < minG) minG = G;
      if (G < limAct99) ++limAct;
      // delay lines (wet = compressed, dry = raw), same length
      float* wp = &wetBuf[(size_t)dpos * C]; float* dp = &dryBuf[(size_t)dpos * C];
      const float bc = bCur;
      float opk = 0.f;
      for (uint32_t c = 0; c < C; ++c) {
        const float wet = wp[c] * G, dry = dp[c];
        wp[c] = cv[c]; dp[c] = x[c];
        const float o = bc <= 0.f ? wet : (bc >= 1.f ? dry : wet * (1.f - bc) + dry * bc);
        out[f * C + c] = o;
        const float a = o < 0 ? -o : o; if (a > opk) opk = a;
      }
      if (opk > outPk) outPk = opk;
      dpos = (dpos + 1 >= D) ? 0 : dpos + 1;
      if (bTgt > 0.5f) ++byAct;
    }
    frames += frames_;
    grNow = gr; grMax = std::max(grMax, grMaxBlk);
    limNow = G < 1.f ? -20.f * std::log10(G) : 0.f;
    if (minG < 1.f) limMaxDb = std::max(limMaxDb, -20.f * std::log10(minG));
  }
};

Dynamics::Dynamics(uint32_t rate, uint32_t channels)
    : p_(new Impl(rate ? rate : 48000, channels < 1 ? 1 : (channels > 16 ? 16 : channels))) {}
Dynamics::~Dynamics() = default;
uint32_t Dynamics::delaySamples() const { return p_->D; }
const DynamicsConfig& Dynamics::activeConfig() const { return p_->cfg; }

uint64_t Dynamics::stageConfig(const DynamicsConfig& c) {
  Impl& s = *p_;
  const uint64_t rev = s.revCounter.fetch_add(1) + 1;
  s.slots[s.back] = c.clamped(); s.slotRev[s.back] = rev;
  s.back = s.mid.exchange(s.back | 4) & 3;
  return rev;
}

void Dynamics::process(const float* in, float* out, size_t frames) { p_->run(in, out, frames); }

bool Dynamics::process(const AudioBlock& in, AudioBlock& out) {
  if (in.rate != p_->rate || in.channels != p_->ch) return false;
  out.rate = in.rate; out.channels = in.channels; out.firstSample = in.firstSample; out.captureNs = in.captureNs;
  out.flags = in.flags; out.generation = in.generation; out.sequence = in.sequence;
  if (out.samples.size() != in.samples.size()) out.samples.resize(in.samples.size());
  p_->run(in.samples.data(), out.samples.data(), in.frames());
  return true;
}

DynamicsTelemetry Dynamics::telemetry() {
  Impl& s = *p_;
  DynamicsTelemetry t;
  t.compGrDb = s.grNow; t.limGrDb = s.limNow; t.maxCompGrDb = s.grMax; t.maxLimGrDb = s.limMaxDb;
  t.inPeakDb = s.inPk > 1e-6f ? 20.f * std::log10(s.inPk) : kDbSilence;
  t.outPeakDb = s.outPk > 1e-6f ? 20.f * std::log10(s.outPk) : kDbSilence;
  s.inPk = s.outPk = 0.f;
  t.bypassed = s.bTgt > 0.5f; t.configRevision = s.cfgRev; t.frames = s.frames;
  t.compActiveFrames = s.compAct; t.limActiveFrames = s.limAct; t.bypassedFrames = s.byAct;
  t.sanitized = s.sanitized; t.delaySamples = s.D;
  return t;
}

void Dynamics::reset() { p_->resetState(); }

} // namespace ss
