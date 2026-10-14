// MOD-03 dynamics tests (REQ-R02, IF-04, spec §12 acceptance vectors).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
#include "test.h"
#include "test_dsp_util.h"
#include "core/dynamics/dynamics.h"
#include <cstdlib>
#include <new>
#include <thread>
#include <atomic>
using namespace ss;
using namespace dsptest;

// ---- allocation counter: proves process() is heap-free ------------------------------------------------
static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_count{false};
void* operator new(std::size_t n) { if (g_count.load()) ++g_allocs; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { if (g_count.load()) ++g_allocs; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr uint32_t R = 48000;

DynamicsConfig neutral() {                 // unity: no trim, ratio 1, limiter off
  DynamicsConfig c; c.ratio = 1.f; c.limiterOn = false; c.makeupDb = 0; c.trimDb = 0; return c;
}

// Process mono/stereo interleaved through a Dynamics in 480-frame blocks.
std::vector<float> run(Dynamics& d, const std::vector<float>& in, uint32_t ch, size_t blk = 480) {
  std::vector<float> out(in.size());
  const size_t frames = in.size() / ch;
  for (size_t p = 0; p < frames; p += blk) {
    const size_t n = std::min(blk, frames - p);
    d.process(in.data() + p * ch, out.data() + p * ch, n);
  }
  return out;
}
double peakDb(const std::vector<float>& v, size_t from = 0) {
  float m = 0; for (size_t i = from; i < v.size(); ++i) m = std::max(m, std::fabs(v[i]));
  return linToDb(m);
}
double rmsDb(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0; for (size_t i = from; i < to; ++i) s += (double)v[i] * v[i];
  return 10.0 * std::log10(s / (to - from) + 1e-30);
}
bool allFinite(const std::vector<float>& v) { for (float x : v) if (!std::isfinite(x)) return false; return true; }

// Reference static curve (spec: soft knee, ratio, threshold) in dB.
double refGr(double L, double T, double ratio, double W) {
  const double s = 1.0 - 1.0 / ratio, o = L - T;
  if (W <= 0) return o > 0 ? s * o : 0;
  if (2 * o < -W) return 0;
  if (2 * std::fabs(o) <= W) return s * (o + W / 2) * (o + W / 2) / (2 * W);
  return s * o;
}

} // namespace

TEST(dyn_delay_and_unity_passthrough_is_bit_exact) {
  for (uint32_t rate : {48000u, 44100u}) {
    Dynamics d(rate, 2);
    CHECK(d.delaySamples() == (uint32_t)std::lround(0.005 * rate));
    d.stageConfig(neutral());
    auto in = dup(noiseBuf(0.5, 1.0, rate, 2));
    auto out = run(d, in, 2);
    const size_t D = d.delaySamples();
    size_t bad = 0;
    for (size_t i = D; i < in.size() / 2; ++i) { if (out[2 * i] != in[2 * (i - D)] || out[2 * i + 1] != in[2 * (i - D) + 1]) ++bad; }
    CHECK(bad == 0);
    for (size_t i = 0; i < D * 2; ++i) CHECK(out[i] == 0.f);
    DynamicsTelemetry t = d.telemetry();
    CHECK(t.delaySamples == D);
  }
}

TEST(dyn_static_compression_curve_within_0p1dB) {
  struct K { float thr, ratio, knee, makeup; };
  const K ks[] = {{-12, 3, 6, 0}, {-12, 3, 0, 0}, {-20, 4, 12, 3}, {-30, 10, 6, 6}, {-6, 2, 0, 0}};
  double worst = 0;
  for (const K& k : ks) {
    for (double L = -50; L <= -1.0; L += 1.0) {
      Dynamics d(R, 1);
      DynamicsConfig c = neutral(); c.thresholdDb = k.thr; c.ratio = k.ratio; c.kneeDb = k.knee; c.makeupDb = k.makeup;
      c.attackMs = 5; c.releaseMs = 100; d.stageConfig(c);
      const double amp = dbToLin(L);
      auto in = sine(1000, amp, 1.2, R);
      auto out = run(d, in, 1);
      const size_t a = (size_t)(0.8 * R), b = (size_t)(1.2 * R) - 240;
      const double gain = rmsDb(out, a + 240, b + 240) - rmsDb(in, a, b);
      const double expect = -refGr(L, k.thr, k.ratio, k.knee) + k.makeup;
      worst = std::max(worst, std::fabs(gain - expect));
      if (std::fabs(gain - expect) > 0.1) std::printf("  curve miss thr %g ratio %g knee %g L %g: gain %.3f expect %.3f\n", k.thr, k.ratio, k.knee, L, gain, expect);
      CHECK_NEAR(gain, expect, 0.1);
    }
  }
  std::printf("  static curve worst error %.4f dB\n", worst);
}

TEST(dyn_limiter_ceiling_within_0p1dB) {
  const double ceilings[] = {-1.0, -0.5, -3.0, -6.0, -12.0};
  for (double ceil : ceilings) {
    for (int sig = 0; sig < 6; ++sig) {
      Dynamics d(R, 2);
      DynamicsConfig c = neutral(); c.limiterOn = true; c.ceilingDb = (float)ceil; c.compressorOn = (sig % 2 == 1);
      c.ratio = 3; c.thresholdDb = -12; d.stageConfig(c);
      std::vector<float> m;
      switch (sig) {
        case 0: case 1: m = sine(997, 1.0, 2.0, R); break;                          // full-scale sine
        case 2: case 3: m = noiseBuf(1.0, 2.0, R, 5); break;                        // full-scale noise
        case 4: { m.assign(R * 2, 0.f); for (size_t i = 3000; i < m.size(); i += 6000) m[i] = 1.0f; break; }   // impulses
        default: { m = sine(200, 4.0, 2.0, R); for (auto& x : m) x = std::max(-1.f, std::min(1.f, x)); break; } // hard clipped
      }
      auto out = run(d, dup(m), 2);
      const double pk = peakDb(out);
      CHECK(pk <= ceil + 0.1);
      if (sig == 0 || sig == 2 || sig == 4 || sig == 5) CHECK(pk >= ceil - 0.1);     // limiter-only: output sits at the ceiling
      if (pk > ceil + 0.1) std::printf("  ceiling %g sig %d peak %.3f\n", ceil, sig, pk);
    }
  }
}

TEST(dyn_limiter_transparent_below_ceiling_and_releases) {
  Dynamics d(R, 1);
  DynamicsConfig c = neutral(); c.limiterOn = true; c.ceilingDb = -1; d.stageConfig(c);
  auto quiet = sine(500, 0.3, 0.5, R);
  auto out = run(d, quiet, 1);
  size_t bad = 0; const size_t D = d.delaySamples();
  for (size_t i = D + 1; i < quiet.size(); ++i) if (out[i] != quiet[i - D]) ++bad;
  CHECK(bad == 0);
  // After a loud burst the gain recovers (release ~100 ms) to unity.
  std::vector<float> seq = sine(500, 3.0, 0.2, R); auto tail = sine(500, 0.3, 1.0, R);
  seq.insert(seq.end(), tail.begin(), tail.end());
  Dynamics e(R, 1); e.stageConfig(c);
  auto o2 = run(e, seq, 1);
  const size_t lastSec = o2.size() - R / 2;
  double mx = 0; for (size_t i = lastSec; i < o2.size(); ++i) mx = std::max(mx, (double)std::fabs(o2[i]));
  CHECK_NEAR(mx, 0.3, 0.003);
  DynamicsTelemetry t = e.telemetry();
  CHECK(t.maxLimGrDb > 10.0f);
}

TEST(dyn_channel_linked_gain) {
  Dynamics d(R, 2);
  DynamicsConfig c = neutral(); c.ratio = 4; c.thresholdDb = -20; c.kneeDb = 0; c.limiterOn = false; d.stageConfig(c);
  auto L = sine(1000, 0.5, 2.0, R), Rr = sine(700, 0.05, 2.0, R);          // L loud (-6 dB), R quiet (-26 dB, below threshold)
  auto out = run(d, toInterleaved(L, Rr), 2);
  std::vector<float> oL(L.size()), oR(L.size());
  for (size_t i = 0; i < L.size(); ++i) { oL[i] = out[2 * i]; oR[i] = out[2 * i + 1]; }
  const size_t a = (size_t)(1.5 * R), b = (size_t)(2.0 * R) - 480;
  const double gL = rmsDb(oL, a + 240, b + 240) - rmsDb(L, a, b);
  const double gR = rmsDb(oR, a + 240, b + 240) - rmsDb(Rr, a, b);
  CHECK(gL < -9.0);                       // compressing: (-6 - -20) * (1 - 1/4) = 10.5 dB
  CHECK_NEAR(gL, gR, 0.05);               // the quiet channel gets the same gain: balance preserved
  DynamicsTelemetry t = d.telemetry();
  CHECK_NEAR(t.compGrDb, 10.5, 0.2);
  // Limiter linking: loud L pulls R down by the same factor.
  Dynamics e(R, 2); DynamicsConfig lc = neutral(); lc.limiterOn = true; lc.ceilingDb = -6; e.stageConfig(lc);
  auto L2 = sine(300, 1.0, 1.0, R), R2 = sine(300, 0.25, 1.0, R);
  auto o2 = run(e, toInterleaved(L2, R2), 2);
  double pl = 0, pr = 0; for (size_t i = R / 2; i < L2.size(); ++i) { pl = std::max(pl, (double)std::fabs(o2[2 * i])); pr = std::max(pr, (double)std::fabs(o2[2 * i + 1])); }
  CHECK_NEAR(linToDb(pl), -6.0, 0.1);
  CHECK_NEAR(linToDb(pr) - linToDb(pl), linToDb(0.25), 0.05);
}

TEST(dyn_no_nan_inf_on_acceptance_vectors) {
  const float nan = std::nanf(""), inf = INFINITY;
  std::vector<std::vector<float>> vecs;
  vecs.push_back(sine(1000, 0.9, 1.0, R));
  { std::vector<float> v(R, 0.f); v[100] = 1.f; v[20000] = -1.f; vecs.push_back(v); }
  { auto v = sine(100, 8.0, 1.0, R); for (auto& x : v) x = std::max(-1.f, std::min(1.f, x)); vecs.push_back(v); }
  vecs.push_back(std::vector<float>(R, 0.f));
  vecs.push_back(noiseBuf(1.0, 1.0, R, 9));
  { std::vector<float> v(R); for (size_t i = 0; i < v.size(); ++i) v[i] = (i & 1) ? 1.f : -1.f; vecs.push_back(v); }     // Nyquist
  { auto v = sine(23900, 1.0, 1.0, R); for (size_t i = 0; i < v.size(); i += 97) v[i] *= 4; vecs.push_back(v); }          // near-Nyquist transients
  { std::vector<float> v(R, 0.f); v[10] = nan; v[11] = inf; v[12] = -inf; v[13] = 3e38f; v[14] = -3e38f; vecs.push_back(v); }
  int idx = 0;
  for (auto& m : vecs) {
    for (bool by : {false, true}) {
      Dynamics d(R, 2);
      DynamicsConfig c; c.bypass = by; c.makeupDb = 6; c.trimDb = 12; d.stageConfig(c);
      auto out = run(d, toInterleaved(m, m), 2);
      CHECK(allFinite(out));
      if (!allFinite(out)) std::printf("  non-finite output for vector %d bypass %d\n", idx, by);
      if (!by) CHECK(peakDb(out) <= -1.0 + 0.1 || idx == 7);
      DynamicsTelemetry t = d.telemetry();
      CHECK(std::isfinite(t.compGrDb) && std::isfinite(t.limGrDb) && std::isfinite(t.outPeakDb));
    }
    ++idx;
  }
  Dynamics d(R, 2);
  std::vector<float> v(R * 2, 0.f); v[5] = nan; v[6] = inf; v[7] = -inf;
  run(d, v, 2);
  CHECK(d.telemetry().sanitized == 3);
}

TEST(dyn_bypass_continuity_and_equal_delay) {
  Dynamics d(R, 2);
  DynamicsConfig c; c.thresholdDb = -30; c.ratio = 8; c.makeupDb = 6; c.trimDb = 6; d.stageConfig(c);   // heavily processing
  const size_t N = R * 3;
  auto m = sine(1000, 0.5, 3.0, R);
  auto in = dup(m);
  std::vector<float> out(in.size());
  const size_t toggle1 = R, toggle2 = 2 * R;
  for (size_t p = 0; p < N; p += 480) {
    if (p == toggle1) { c.bypass = true; d.stageConfig(c); }
    if (p == toggle2) { c.bypass = false; d.stageConfig(c); }
    d.process(in.data() + p * 2, out.data() + p * 2, 480);
  }
  // Max sample-to-sample step of the original sine is 2*pi*1000/48000*0.5 ~ 0.0654 * post gain; the processed
  // path is louder (trim+makeup, compressed) so bound the step against the louder of the two paths.
  double maxStep = 0, maxStepOutsideTransition = 0;
  for (size_t i = 1; i < N; ++i) {
    const double s = std::fabs(out[2 * i] - out[2 * (i - 1)]);
    maxStep = std::max(maxStep, s);
  }
  (void)maxStepOutsideTransition;
  // Step bound: amplitude gain <= ~2.0 from processing (6 dB trim + 6 dB makeup - compression) -> allow generous 0.4 and
  // far below a click (a hard switch between the two paths would jump by ~0.5+).
  CHECK(maxStep < 0.08);   // natural max step of the dry sine is 0.065
  // Fully bypassed region is the exact delayed input.
  const size_t D = d.delaySamples();
  size_t bad = 0;
  for (size_t i = toggle1 + 2000; i < toggle2 - 10; ++i) if (out[2 * i] != in[2 * (i - D)]) ++bad;
  CHECK(bad == 0);
  // Without any staged change a bypass toggle is the only thing that moves the output; delay identical in both states.
  CHECK(d.telemetry().bypassedFrames > 0);
}

TEST(dyn_config_staging_clamp_and_revisions) {
  Dynamics d(R, 1);
  DynamicsConfig c; c.trimDb = 100; c.ratio = -3; c.thresholdDb = std::nanf(""); c.ceilingDb = 5; c.releaseMs = 1e9f;
  const uint64_t r1 = d.stageConfig(c);
  const uint64_t r2 = d.stageConfig(c);
  CHECK(r2 > r1);
  std::vector<float> in(480, 0.1f), out(480);
  d.process(in.data(), out.data(), 480);
  const DynamicsConfig a = d.activeConfig();
  CHECK(a.trimDb == 12.f); CHECK(a.ratio == 1.f); CHECK(a.thresholdDb == -12.f); CHECK(a.ceilingDb == -0.5f); CHECK(a.releaseMs == 2000.f);
  CHECK(d.telemetry().configRevision == r2);
  // Staging from another thread while processing: lock-free hand-off, last revision wins, no corruption.
  std::atomic<bool> stop{false};
  std::thread t([&] { DynamicsConfig k; int i = 0; while (!stop.load()) { k.trimDb = (float)(i++ % 10) - 5.f; k.thresholdDb = -20.f; d.stageConfig(k); } });
  for (int i = 0; i < 2000; ++i) d.process(in.data(), out.data(), 480);
  stop = true; t.join();
  for (float x : out) CHECK(std::isfinite(x));
  const DynamicsConfig b = d.activeConfig();
  CHECK(b.trimDb >= -5.f && b.trimDb <= 4.f && b.thresholdDb == -20.f);
}

TEST(dyn_telemetry_peaks_and_activity) {
  Dynamics d(R, 2);
  DynamicsConfig c; c.thresholdDb = -20; c.ratio = 4; c.ceilingDb = -1; d.stageConfig(c);
  auto m = sine(500, 1.0, 1.0, R);
  run(d, dup(m), 2);
  DynamicsTelemetry t = d.telemetry();
  CHECK_NEAR(t.inPeakDb, 0.0, 0.1);
  CHECK(t.outPeakDb <= -0.9f);
  CHECK(t.compGrDb > 5.f); CHECK(t.maxCompGrDb >= t.compGrDb);
  CHECK(t.compActiveFrames > R / 2); CHECK(t.frames == R);
  CHECK(!t.bypassed);
  // Peak holds are per interval.
  std::vector<float> z(2 * R / 10, 0.f); std::vector<float> o(z.size());
  d.process(z.data(), o.data(), z.size() / 2);
  DynamicsTelemetry u = d.telemetry();
  CHECK(u.inPeakDb == kDbSilence);
  d.reset(); CHECK(d.telemetry().frames == 0);
}

TEST(dyn_process_is_allocation_free) {
  Dynamics d(R, 2);
  DynamicsConfig c; c.bypass = false; d.stageConfig(c);
  auto in = dup(noiseBuf(0.7, 0.2, R, 1)); std::vector<float> out(in.size());
  AudioBlock ib, ob; ib.rate = R; ib.channels = 2; ib.samples = in; ob.samples.resize(in.size());
  d.process(in.data(), out.data(), in.size() / 2);                       // warm
  g_allocs = 0; g_count = true;
  for (int i = 0; i < 20; ++i) {
    d.process(in.data(), out.data(), in.size() / 2);
    d.process(ib, ob);
    if (i == 5) { c.bypass = true; d.stageConfig(c); }
    if (i == 10) { c.bypass = false; c.thresholdDb = -30; d.stageConfig(c); }
    (void)d.telemetry();
  }
  g_count = false;
  CHECK(g_allocs.load() == 0);
}

int main() { return ss_test::runAll(); }
