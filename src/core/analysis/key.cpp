#include "core/analysis/key.h"
#include <cmath>
#include <algorithm>

namespace ss {
namespace analysis {

namespace {
constexpr int kN = 2048;
constexpr double kFLo = 65.0, kFHi = 1000.0;
constexpr double kTau = 10.0;            // context time constant (~8-20 s effective)
constexpr double kMinSpan = 8.0;         // seconds of non-silent evidence before a key may be shown
// Temperley (2005, Kostka-Payne) profiles: behave better than Temperley on harmonic-rich bare triads.
const double kMajor[12] = {0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400};
const double kMinor[12] = {0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330};
// Reporting thresholds (provisional; see spec §12 - to be calibrated on an annotated corpus).
constexpr float kMinR = 0.75f, kMinConf = 0.35f;
}

KeyEstimator::KeyEstimator(double inRate) : fk_(inRate / 8.0), fft_(kN) {
  win_.resize(kN); ring_.assign(2 * kN, 0.f);
  const double tp = 6.283185307179586;
  for (int j = 0; j < kN; ++j) win_[j] = (float)(0.5 - 0.5 * std::cos(tp * j / kN));
  const double df = fk_ / kN;
  kLo_ = (int)std::ceil(kFLo / df);
  kHi_ = (int)std::floor(std::min(kFHi, fk_ * 0.45) / df);
  pwr_.assign(kN / 2 + 1, 0.f); mag_.assign(kN / 2 + 1, 0.f); cum_.assign(kN / 2 + 2, 0.f);
  frameSec_ = 2.0 * kN / fk_;
  decay_ = std::exp(-frameSec_ / kTau);
  reset();
}

void KeyEstimator::reset() {
  for (auto& h : hb_) h.reset();
  std::fill(ring_.begin(), ring_.end(), 0.f);
  rpos_ = 0; sinceFrame_ = 0; total_ = 0;
  for (double& c : chroma_) c = 0;
  wAcc_ = 0; spanSec_ = 0; silentSec_ = 0;
  valid_ = false; conf_ = 0; pendKey_ = -1; pendCount_ = 0;
}

void KeyEstimator::flushStream() {
  for (auto& h : hb_) h.reset();
  std::fill(ring_.begin(), ring_.end(), 0.f);
  rpos_ = 0; sinceFrame_ = 0; total_ = 0;
}

void KeyEstimator::push(float x) {
  float y = x;
  for (int s = 0; s < 3; ++s) { float o; if (!hb_[s].push(y, o)) return; y = o; }
  ring_[rpos_] = y; rpos_ = (rpos_ + 1) & (2 * kN - 1);
  ++total_;
  if (++sinceFrame_ >= 2 * kN) { sinceFrame_ = 0; frame(); }   // two contiguous 2048 windows per frame
}

void KeyEstimator::frame() {
  float* re = fft_.re(); float* im = fft_.im(); const int* rev = fft_.rev();
  double eA = 0, eB = 0;
  for (int j = 0; j < kN; ++j) {
    const float a = ring_[(rpos_ + j) & (2 * kN - 1)];
    const float b = ring_[(rpos_ + kN + j) & (2 * kN - 1)];
    eA += (double)a * a; eB += (double)b * b;
    re[rev[j]] = a * win_[j]; im[rev[j]] = b * win_[j];
  }
  const double ms = (eA + eB) / (2.0 * kN);
  if (ms < 2.5e-7) {                         // silent frame (< ~-66 dBFS)
    silentSec_ += frameSec_;
    if (silentSec_ >= 4.0) reset();
    return;
  }
  silentSec_ = 0;
  fft_.run();
  const int k0 = std::max(2, kLo_ - 16), k1 = std::min(kN / 2 - 2, kHi_ + 16);
  for (int k = k0 - 1; k <= k1 + 1; ++k) {
    const float p = 0.5f * (re[k] * re[k] + im[k] * im[k] + re[kN - k] * re[kN - k] + im[kN - k] * im[kN - k]);
    pwr_[k] = p; mag_[k] = std::sqrt(p);
  }
  cum_[k0 - 1] = 0;
  for (int k = k0; k <= k1; ++k) cum_[k] = cum_[k - 1] + pwr_[k];
  double fc[12] = {0}; double pm = 0, tm = 0;
  const double df = fk_ / kN;
  for (int k = kLo_; k <= kHi_; ++k) {
    tm += mag_[k];
    const float p = pwr_[k];
    if (!(p > pwr_[k - 1] && p >= pwr_[k + 1])) continue;
    const int a = std::max(k0, k - 16), b = std::min(k1, k + 16);
    const float mean = (cum_[b] - cum_[a - 1]) / (float)(b - a + 1);
    if (p < 6.0f * mean) continue;
    const double ma = mag_[k - 1], mb = mag_[k], mc = mag_[k + 1];
    const double d = ma - 2 * mb + mc;
    const double delta = d < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (ma - mc) / d)) : 0.0;
    const double f = (k + delta) * df;
    double midi = 69.0 + 12.0 * std::log2(f / 440.0);
    double pcf = std::fmod(midi, 12.0); if (pcf < 0) pcf += 12.0;
    const int lo = (int)pcf; const double w = pcf - lo;
    fc[lo] += mb * (1.0 - w); fc[(lo + 1) % 12] += mb * w;
    pm += mb;
  }
  const double tonality = tm > 0 ? pm / tm : 0.0;
  for (double& c : chroma_) c *= decay_;
  wAcc_ *= decay_;
  if (pm > 0) {
    const double w = tonality;
    for (int i = 0; i < 12; ++i) chroma_[i] += w * fc[i] / pm;
    wAcc_ += w;
  }
  spanSec_ += frameSec_;
  evaluate();
}

void KeyEstimator::evaluate() {
  if (spanSec_ < kMinSpan) return;
  double sum = 0; for (double c : chroma_) sum += c;
  const double meanTon = wAcc_ * (1.0 - decay_);          // steady-state mean tonality weight
  if (sum <= 1e-9 || meanTon < 0.08) { if (valid_) { conf_ *= 0.7f; if (conf_ < kMinConf * 0.6f) valid_ = false; } return; }
  double c[12]; double cm = 0;
  for (int i = 0; i < 12; ++i) { c[i] = chroma_[i] / sum; cm += c[i]; }
  cm /= 12.0;
  double cv = 0; for (int i = 0; i < 12; ++i) cv += (c[i] - cm) * (c[i] - cm);
  double score[24];
  for (int mode = 0; mode < 2; ++mode) {
    const double* prof = mode ? kMinor : kMajor;
    double pmn = 0; for (int i = 0; i < 12; ++i) pmn += prof[i]; pmn /= 12;
    double pv = 0; for (int i = 0; i < 12; ++i) pv += (prof[i] - pmn) * (prof[i] - pmn);
    for (int tonic = 0; tonic < 12; ++tonic) {
      double s = 0;
      for (int i = 0; i < 12; ++i) s += (c[(tonic + i) % 12] - cm) * (prof[i] - pmn);
      score[mode * 12 + tonic] = (cv > 0 && pv > 0) ? s / std::sqrt(cv * pv) : 0.0;
    }
  }
  int b1 = 0; for (int i = 1; i < 24; ++i) if (score[i] > score[b1]) b1 = i;
  double r2 = -1; for (int i = 0; i < 24; ++i) if (i != b1 && score[i] > r2) r2 = score[i];
  const float r1 = (float)score[b1], margin = (float)(score[b1] - r2);
  lastR_ = r1; lastMargin_ = margin;
  float conf = std::min(1.f, std::max(0.f, (r1 - 0.55f) / 0.35f)) * std::min(1.f, std::max(0.f, margin / 0.08f));
  if (r1 < kMinR || conf < kMinConf) {
    if (valid_) { conf_ *= 0.7f; if (conf_ < kMinConf * 0.6f) valid_ = false; }
    pendKey_ = -1; pendCount_ = 0;
    return;
  }
  if (!valid_) {
    valid_ = true; pc_ = b1 % 12; minor_ = b1 >= 12; conf_ = conf; ++updates_; pendKey_ = -1; pendCount_ = 0;
    return;
  }
  const int cur = pc_ + (minor_ ? 12 : 0);
  if (b1 == cur) { conf_ += 0.5f * (conf - conf_); ++updates_; pendKey_ = -1; pendCount_ = 0; return; }
  if (b1 == pendKey_) ++pendCount_; else { pendKey_ = b1; pendCount_ = 1; }
  conf_ *= 0.8f;                                           // old confidence clears gradually
  if (pendCount_ >= 2 || conf_ < kMinConf) { pc_ = b1 % 12; minor_ = b1 >= 12; conf_ = conf; ++updates_; pendKey_ = -1; pendCount_ = 0; }
}

} // namespace analysis
} // namespace ss
