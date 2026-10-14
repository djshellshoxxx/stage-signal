// Shared synthetic-signal helpers for MOD-03 DSP tests (deterministic; no files, no sleeps).
#pragma once
#include "core/types.h"
#include "core/analysis/analyzer.h"
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace dsptest {

constexpr double kPi = 3.14159265358979323846;

struct Lcg {                       // deterministic uniform/noise source
  uint32_t s;
  explicit Lcg(uint32_t seed = 1) : s(seed) {}
  uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
  double uni() { return (next() >> 8) / 16777216.0; }          // [0,1)
  double noise() { return uni() * 2.0 - 1.0; }                  // [-1,1)
};

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double a) { return a > 1e-12 ? 20.0 * std::log10(a) : -240.0; }

// Feed interleaved samples to the analyzer in blocks of `blockFrames`; calls onBlock(endFrameIndex) after each.
template <class F>
inline void feed(ss::Analyzer& a, const std::vector<float>& inter, uint32_t rate, uint32_t ch,
                 size_t blockFrames, F onBlock, uint64_t firstSample0 = 0, uint32_t gen = 0) {
  const size_t total = inter.size() / ch;
  ss::AudioBlock b; b.rate = rate; b.channels = ch; b.generation = gen;
  b.flags = ss::AF_TimestampValid;
  for (size_t pos = 0; pos < total; pos += blockFrames) {
    const size_t n = std::min(blockFrames, total - pos);
    b.samples.assign(inter.begin() + pos * ch, inter.begin() + (pos + n) * ch);
    b.firstSample = firstSample0 + pos;
    b.captureNs = (ss::Ns)((double)(firstSample0 + pos) * 1e9 / rate);
    b.sequence++;
    a.process(b);
    onBlock(pos + n);
  }
}
inline void feed(ss::Analyzer& a, const std::vector<float>& inter, uint32_t rate, uint32_t ch, size_t blockFrames = 480) {
  feed(a, inter, rate, ch, blockFrames, [](size_t) {});
}
inline ss::Ns ns(size_t frames, uint32_t rate) { return (ss::Ns)((double)frames * 1e9 / rate); }

inline std::vector<float> toInterleaved(const std::vector<float>& L, const std::vector<float>& R) {
  std::vector<float> o(L.size() * 2);
  for (size_t i = 0; i < L.size(); ++i) { o[2 * i] = L[i]; o[2 * i + 1] = R[i]; }
  return o;
}
inline std::vector<float> dup(const std::vector<float>& m) { return toInterleaved(m, m); }
inline std::vector<float> sine(double f, double amp, double sec, uint32_t rate, double phase = 0) {
  std::vector<float> v((size_t)(sec * rate));
  for (size_t i = 0; i < v.size(); ++i) v[i] = (float)(amp * std::sin(2 * kPi * f * i / rate + phase));
  return v;
}
inline std::vector<float> noiseBuf(double amp, double sec, uint32_t rate, uint32_t seed) {
  Lcg r(seed); std::vector<float> v((size_t)(sec * rate));
  for (auto& x : v) x = (float)(amp * r.noise());
  return v;
}
// Add a decaying burst (sine at f + optional noise) starting at sample `at`.
inline void addBurst(std::vector<float>& v, size_t at, double f, double amp, double tauSec, uint32_t rate, double noiseMix, Lcg& r) {
  const size_t len = (size_t)(tauSec * 6 * rate);
  for (size_t i = 0; i < len && at + i < v.size(); ++i) {
    const double t = (double)i / rate, env = std::exp(-t / tauSec);
    v[at + i] += (float)(amp * env * ((1.0 - noiseMix) * std::sin(2 * kPi * f * t) + noiseMix * r.noise()));
  }
}

} // namespace dsptest
