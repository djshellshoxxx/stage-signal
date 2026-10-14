// MOD-03 micro-benchmark: analyzer and dynamics CPU cost per second of audio (spec §7 BASE budget).
// Gate: < 10% of an 800 MHz core (80M cycles per audio second); design target < 6M cycles.
#include "test.h"
#include "test_dsp_util.h"
#include "core/dynamics/dynamics.h"
#include <chrono>
#include <cstdlib>
#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#define SS_HAVE_TSC 1
#endif
using namespace ss;
using namespace dsptest;

namespace {

std::vector<float> musicLike(double sec, uint32_t rate) {
  std::vector<float> v = noiseBuf(0.01, sec, rate, 3);
  Lcg r(5);
  for (double t = 0.2; t < sec - 0.5; t += 0.46875) addBurst(v, (size_t)(t * rate), 80, 0.7, 0.05, rate, 0.0, r);     // kick @128 BPM
  for (double t = 0.4; t < sec - 0.5; t += 0.9375) addBurst(v, (size_t)(t * rate), 0, 0.4, 0.04, rate, 1.0, r);      // snare
  const double notes[] = {220.0, 277.18, 329.63, 440.0};
  for (int k = 0; k < 4; ++k) for (size_t i = 0; i < v.size(); ++i) v[i] += (float)(0.05 * std::sin(2 * kPi * notes[k] * i / rate));
  return v;
}

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

} // namespace

TEST(dsp_bench_analyzer_cost_per_audio_second) {
  const uint32_t rates[] = {48000, 44100};
  for (uint32_t rate : rates) {
    const double sec = 30.0;
    auto mono = musicLike(sec, rate);
    auto in = dup(mono);
    Analyzer a(rate, 2);
    AudioBlock b; b.rate = rate; b.channels = 2; b.flags = AF_TimestampValid;
    const size_t frames = in.size() / 2, blk = 480;
    b.samples.resize(blk * 2);
    double best = 1e30; unsigned long long bestCyc = 0;
    for (int rep = 0; rep < 3; ++rep) {
      a.reset();
      const double t0 = nowSec();
#ifdef SS_HAVE_TSC
      const unsigned long long c0 = __rdtsc();
#endif
      for (size_t p = 0; p + blk <= frames; p += blk) {
        std::copy(in.begin() + p * 2, in.begin() + (p + blk) * 2, b.samples.begin());
        b.firstSample = p; b.captureNs = ns(p, rate);
        a.process(b);
        if ((p / blk) % 20 == 0) (void)a.snapshot(b.captureNs);
      }
#ifdef SS_HAVE_TSC
      const unsigned long long c1 = __rdtsc();
#endif
      const double dt = nowSec() - t0;
      if (dt < best) {
        best = dt;
#ifdef SS_HAVE_TSC
        bestCyc = c1 - c0;
#endif
      }
    }
    const double msPerSec = 1000.0 * best / sec;
    std::printf("  [bench] analyzer %u Hz stereo: %.3f ms per audio second (%.3f%% of one core here)", rate, msPerSec, 0.1 * msPerSec);
    if (bestCyc) std::printf(", ~%.2f M TSC cycles per audio second", bestCyc / sec / 1e6);
    std::printf("\n");
    if (bestCyc) {
      const double mc = bestCyc / sec / 1e6;
      std::printf("  [bench]   => %.2f%% of an 800 MHz core (gate 10%%, target < 6M cycles = 0.75%%)\n", mc / 800.0 * 100.0);
      CHECK(mc < 80.0);
    }
    CHECK(msPerSec < 100.0 * 0.1 * 10);   // sanity ceiling on any host: < 10% of a core at 1 s/s would be 100 ms
  }
}

TEST(dsp_bench_dynamics_cost_per_audio_second) {
  const uint32_t rate = 48000; const double sec = 20.0;
  auto mono = musicLike(sec, rate); auto in = dup(mono);
  std::vector<float> out(in.size());
  Dynamics d(rate, 2);
  const size_t frames = in.size() / 2, blk = 480;
  double best = 1e30; unsigned long long bestCyc = 0;
  for (int rep = 0; rep < 3; ++rep) {
    d.reset();
    const double t0 = nowSec();
#ifdef SS_HAVE_TSC
    const unsigned long long c0 = __rdtsc();
#endif
    for (size_t p = 0; p + blk <= frames; p += blk) d.process(in.data() + p * 2, out.data() + p * 2, blk);
#ifdef SS_HAVE_TSC
    const unsigned long long c1 = __rdtsc();
#endif
    const double dt = nowSec() - t0;
    if (dt < best) { best = dt;
#ifdef SS_HAVE_TSC
      bestCyc = c1 - c0;
#endif
    }
  }
  std::printf("  [bench] dynamics 48 kHz stereo: %.3f ms per audio second", 1000.0 * best / sec);
  if (bestCyc) std::printf(", ~%.2f M TSC cycles per audio second", bestCyc / sec / 1e6);
  std::printf("\n");
  if (bestCyc) CHECK(bestCyc / sec / 1e6 < 80.0);
}

int main() { return ss_test::runAll(); }
