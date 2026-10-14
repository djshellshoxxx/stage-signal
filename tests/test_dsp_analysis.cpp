// MOD-03 analyzer tests: levels, bands, onsets, clipping, sanitation, discontinuity (REQ-A03/A04/A10, IF-01/03).
#include "test.h"
#include "test_dsp_util.h"
#include <cstdlib>
using namespace ss;
using namespace dsptest;

namespace {

// Run a tone through the analyzer, return final snapshot.
FeatureSnapshot runTone(double f, double ampLin, uint32_t rate, uint32_t ch, double sec = 1.5) {
  Analyzer a(rate, ch);
  auto m = sine(f, ampLin, sec, rate);
  std::vector<float> in = ch == 2 ? dup(m) : m;
  feed(a, in, rate, ch);
  return a.snapshot(ns(m.size(), rate));
}

int bandOf(const FeatureSnapshot& s) {
  int b = 0; for (int i = 1; i < 3; ++i) if (s.bandDb[i] > s.bandDb[b]) b = i;
  return b;
}

} // namespace

TEST(dsp_tone_rms_and_band) {
  struct Case { double f; int band; };
  const Case cases[] = {{100, 0}, {150, 0}, {500, 1}, {1000, 1}, {4000, 2}, {8000, 2}};
  const uint32_t rates[] = {48000, 44100};
  for (uint32_t rate : rates)
    for (uint32_t ch = 1; ch <= 2; ++ch)
      for (const Case& c : cases) {
        const double amp = 0.1;                               // -20 dBFS peak => -23.01 dBFS RMS
        FeatureSnapshot s = runTone(c.f, amp, rate, ch);
        const double expRms = linToDb(amp / std::sqrt(2.0));
        CHECK_NEAR(s.rmsDb[0], expRms, 0.5);
        CHECK_NEAR(s.rmsDb[1], expRms, 0.5);
        CHECK_NEAR(s.rmsDbMix, expRms, 0.5);
        CHECK(s.peakDb[0] <= linToDb(amp) + 0.1 && s.peakDb[0] >= linToDb(amp) - 1.5);   // sample peak of a sampled sine
        CHECK(bandOf(s) == c.band);
        // Expected band carries the tone's power (same units as rms), other bands far below.
        CHECK_NEAR(s.bandDb[c.band], expRms, 1.5);
        for (int b = 0; b < 3; ++b) if (b != c.band) CHECK(s.bandDb[b] < expRms - 30.0);
        CHECK(!s.silent);
        if (bandOf(s) != c.band || std::fabs(s.rmsDb[0] - expRms) > 0.5)
          std::printf("  (rate %u ch %u f %g: rms %.2f bands %.1f %.1f %.1f)\n", rate, ch, c.f, s.rmsDb[0], s.bandDb[0], s.bandDb[1], s.bandDb[2]);
      }
}

TEST(dsp_stereo_channels_independent) {
  Analyzer a(48000, 2);
  auto L = sine(1000, 0.2, 1.5, 48000), R = sine(100, 0.05, 1.5, 48000);
  feed(a, toInterleaved(L, R), 48000, 2);
  FeatureSnapshot s = a.snapshot(0);
  CHECK_NEAR(s.rmsDb[0], linToDb(0.2 / std::sqrt(2.0)), 0.5);
  CHECK_NEAR(s.rmsDb[1], linToDb(0.05 / std::sqrt(2.0)), 0.5);
  // Anti-phase stereo must not cancel in the band analysis (energies are summed per channel).
  Analyzer b(48000, 2);
  auto m = sine(1000, 0.1, 1.5, 48000); std::vector<float> n(m.size());
  for (size_t i = 0; i < m.size(); ++i) n[i] = -m[i];
  feed(b, toInterleaved(m, n), 48000, 2);
  FeatureSnapshot t = b.snapshot(0);
  CHECK_NEAR(t.bandDb[1], linToDb(0.1 / std::sqrt(2.0)), 1.5);
}

TEST(dsp_silence_and_dc_excluded) {
  Analyzer a(48000, 2);
  feed(a, std::vector<float>(48000 * 2 * 2, 0.f), 48000, 2);
  FeatureSnapshot s = a.snapshot(0);
  CHECK(s.silent); CHECK(s.rmsDbMix <= kDbSilence + 0.1f);
  CHECK(!s.tempo.valid); CHECK(!s.key.valid); CHECK(s.onsetCount == 0);
  CHECK(!s.inputNearFullScale && !s.possibleClipping);
  // DC (0.5) must not appear in any band.
  Analyzer b(48000, 1);
  feed(b, std::vector<float>(48000 * 2, 0.5f), 48000, 1);
  FeatureSnapshot d = b.snapshot(0);
  for (int i = 0; i < 3; ++i) CHECK(d.bandDb[i] < -50.0f);
}

TEST(dsp_clipping_flags_distinguish_input_warning) {
  // Isolated near-full-scale peaks: warning but not "possible clipping".
  Analyzer a(48000, 1);
  auto v = sine(200, 0.3, 1.0, 48000);
  v[10000] = 0.97f; v[20000] = -0.99f;
  feed(a, v, 48000, 1);
  FeatureSnapshot s = a.snapshot(0);
  CHECK(s.inputNearFullScale); CHECK(!s.possibleClipping);
  // Clamped (flat-topped) sine: consecutive near-full-scale samples -> possible clipping.
  Analyzer b(48000, 1);
  auto c = sine(200, 1.6, 1.0, 48000);
  for (auto& x : c) x = std::max(-1.f, std::min(1.f, x));
  feed(b, c, 48000, 1);
  FeatureSnapshot t = b.snapshot(0);
  CHECK(t.inputNearFullScale); CHECK(t.possibleClipping);
  // Level just under -0.5 dBFS: no warning.
  Analyzer d(48000, 1);
  feed(d, sine(200, 0.93, 1.0, 48000), 48000, 1);
  CHECK(!d.snapshot(0).inputNearFullScale);
  // Flags clear after ~1-2 s of normal signal.
  feed(d, sine(200, 0.1, 3.0, 48000), 48000, 1);
  FeatureSnapshot e = b.snapshot(0); (void)e;
  feed(b, sine(200, 0.1, 3.0, 48000), 48000, 1);
  FeatureSnapshot f = b.snapshot(0);
  CHECK(!f.inputNearFullScale); CHECK(!f.possibleClipping);
}

TEST(dsp_sanitizes_nan_inf) {
  Analyzer a(48000, 2);
  auto v = dup(sine(440, 0.2, 1.0, 48000));
  const float nan = std::nanf(""), inf = INFINITY;
  v[100] = nan; v[201] = inf; v[300] = -inf; v[5000] = nan;
  feed(a, v, 48000, 2);
  FeatureSnapshot s = a.snapshot(0);
  CHECK(a.counters().sanitized == 4);
  CHECK(std::isfinite(s.rmsDb[0]) && std::isfinite(s.rmsDbMix));
  for (int i = 0; i < 3; ++i) CHECK(std::isfinite(s.bandDb[i]) && std::isfinite(s.band[i]));
  CHECK(std::isfinite(s.onsetStrength));
  // Pure garbage stays finite.
  Analyzer b(48000, 1);
  std::vector<float> g(48000, nan);
  feed(b, g, 48000, 1);
  CHECK(std::isfinite(b.snapshot(0).rmsDbMix));
  CHECK(b.counters().sanitized == 48000);
}

TEST(dsp_full_scale_noise_and_near_nyquist_finite) {
  Analyzer a(48000, 2);
  auto n = noiseBuf(1.0, 2.0, 48000, 7);
  feed(a, dup(n), 48000, 2);
  FeatureSnapshot s = a.snapshot(0);
  CHECK(std::isfinite(s.rmsDbMix) && s.inputNearFullScale);
  for (int i = 0; i < 3; ++i) CHECK(std::isfinite(s.bandDb[i]));
  Analyzer b(44100, 1);
  auto t = sine(22000, 1.0, 1.0, 44100);
  feed(b, t, 44100, 1);
  FeatureSnapshot u = b.snapshot(0);
  CHECK(std::isfinite(u.rmsDbMix));
  for (int i = 0; i < 3; ++i) CHECK(std::isfinite(u.bandDb[i]) && u.band[i] >= 0 && u.band[i] <= 1);
}

TEST(dsp_discontinuity_and_generation) {
  Analyzer a(48000, 1);
  auto v = sine(1000, 0.2, 1.0, 48000);
  feed(a, v, 48000, 1);
  CHECK(a.counters().discontinuities == 0);
  // A discontinuity block (capture overrun): analysis state is flushed but analysis continues.
  AudioBlock b; b.rate = 48000; b.channels = 1; b.flags = AF_Discontinuity | AF_TimestampValid;
  b.firstSample = 48000 + 9600; b.samples.assign(4800, 0.f); b.captureNs = ns(b.firstSample, 48000);
  a.process(b);
  CHECK(a.counters().discontinuities == 1);
  feed(a, sine(1000, 0.2, 1.0, 48000), 48000, 1, 480, [](size_t) {}, 48000 + 9600 + 4800);
  FeatureSnapshot s = a.snapshot(ns(100000, 48000));
  CHECK_NEAR(s.rmsDb[0], linToDb(0.2 / std::sqrt(2.0)), 0.5);
  CHECK(bandOf(s) == 1);
  // Stale generation dropped; mismatched format rejected; newer generation resets.
  AudioBlock st = b; st.flags = 0; st.generation = 0; st.samples.assign(480, 0.5f);
  AudioBlock nw = st; nw.generation = 1;
  a.process(nw);
  a.process(st);
  CHECK(a.counters().rejectedBlocks == 1);
  AudioBlock bad = nw; bad.channels = 2; a.process(bad);
  CHECK(a.counters().rejectedBlocks == 2);
  a.reset();
  CHECK(a.counters().blocks == 0);
  CHECK(a.snapshot(0).silent);
}

// ---- Onset corpus (REQ-A04) -----------------------------------------------------------------------
namespace {

struct Corpus { std::vector<float> sig; std::vector<double> truth; };

// Pulses at pseudo-random intervals (0.25..0.9 s) of a given timbre over a quiet noise floor.
Corpus makePulses(int kind, uint32_t seed, double sec, uint32_t rate) {
  Corpus c; c.sig = noiseBuf(0.002, sec, rate, seed + 99);          // ~-54 dBFS floor
  Lcg r(seed);
  double t = 0.5;
  while (t < sec - 0.5) {
    const size_t at = (size_t)(t * rate);
    switch (kind) {
      case 0: addBurst(c.sig, at, 1200, 0.5, 0.012, rate, 0.2, r); break;   // click / rim
      case 1: addBurst(c.sig, at, 70, 0.7, 0.04, rate, 0.0, r); break;      // kick
      case 2: addBurst(c.sig, at, 0, 0.6, 0.03, rate, 1.0, r); break;       // snare-ish noise burst
      default: addBurst(c.sig, at, 3000, 0.4, 0.005, rate, 0.6, r); break;  // hat
    }
    c.truth.push_back(t);
    t += 0.25 + 0.65 * r.uni();
  }
  return c;
}

} // namespace

TEST(dsp_onset_corpus_recall_and_false_rate) {
  const uint32_t rates[] = {48000, 44100};
  size_t totalTruth = 0, totalMatched = 0, totalDet = 0, totalFalse = 0;
  for (uint32_t rate : rates)
    for (int kind = 0; kind < 4; ++kind) {
      Corpus c = makePulses(kind, 11 + kind, 30.0, rate);
      Analyzer a(rate, 1);
      std::vector<double> det;
      uint64_t seen = 0;
      feed(a, c.sig, rate, 1, 240, [&](size_t endFrames) {
        FeatureSnapshot s = a.snapshot(ns(endFrames, rate));
        if (s.onsetCount != seen) { seen = s.onsetCount; det.push_back((double)endFrames / rate); }
      });
      std::vector<bool> used(det.size(), false);
      size_t matched = 0; double offSum = 0;
      for (double t : c.truth) {
        int best = -1; double bd = 0.050;
        for (size_t i = 0; i < det.size(); ++i) {
          if (used[i]) continue;
          const double d = std::fabs(det[i] - t);
          if (d <= bd) { bd = d; best = (int)i; }
        }
        if (best >= 0) { used[best] = true; ++matched; offSum += det[best] - t; }
      }
      const size_t falseN = det.size() - matched;
      if (getenv("DSP_DEBUG")) for (size_t i = 0; i < det.size(); ++i) if (!used[i]) {
        double nd = 1e9; for (double tt : c.truth) if (std::fabs(det[i]-tt) < std::fabs(nd)) nd = det[i]-tt;
        std::printf("    false det at %.3f (nearest truth %+.3f)\n", det[i], nd); }
      std::printf("  onset corpus kind %d rate %u: truth %zu matched %zu det %zu false %zu meanLatency %.1f ms\n",
                  kind, rate, c.truth.size(), matched, det.size(), falseN, matched ? 1000 * offSum / matched : 0.0);
      totalTruth += c.truth.size(); totalMatched += matched; totalDet += det.size(); totalFalse += falseN;
      CHECK(matched >= 0.95 * c.truth.size());
      CHECK(falseN <= 0.05 * std::max<size_t>(1, det.size()));
    }
  std::printf("  onset corpus total: recall %.1f%% false %.1f%%\n", 100.0 * totalMatched / totalTruth, 100.0 * totalFalse / std::max<size_t>(1, totalDet));
}

TEST(dsp_steady_signals_no_onsets) {
  Analyzer a(48000, 2);
  feed(a, dup(sine(440, 0.3, 5.0, 48000)), 48000, 2);
  CHECK(a.snapshot(0).onsetCount <= 1);          // only the initial attack from silence may count
  Analyzer b(48000, 1);
  feed(b, noiseBuf(0.1, 10.0, 48000, 3), 48000, 1);
  CHECK(b.snapshot(0).onsetCount <= 1);
}

int main() { return ss_test::runAll(); }
