#include "core/analysis/tempo.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ss {
namespace analysis {

namespace {
constexpr double kWindowSec = 7.0;      // novelty context
constexpr double kOnsetCtxSec = 10.0;   // onset-time context
constexpr int kOnsetCap = 256;
constexpr float kValidConf = 0.40f;
}

TempoEstimator::TempoEstimator(double fr) : fr_(fr), envRate_(fr / 2.0) {
  lenv_ = (int)std::ceil(kWindowSec * envRate_);
  lagMin_ = (int)std::floor(60.0 / kMaxBpm * envRate_);
  lagMax_ = (int)std::ceil(60.0 / kMinBpm * envRate_);
  if (lagMin_ < 2) lagMin_ = 2;
  maxLag_ = lagMax_ * 2 + 2;
  env_.assign(lenv_, 0.f); lin_.assign(lenv_, 0.f); lin2_.assign(lenv_, 0.f); acf_.assign(maxLag_ + 2, 0.f); sm_.assign(maxLag_ + 2, 0.f); sc_.assign(lagMax_ - lagMin_ + 4, 0.0);
  onT_.assign(kOnsetCap, 0.0); onW_.assign(kOnsetCap, 0.f);
  reset();
}

void TempoEstimator::reset() {
  std::fill(env_.begin(), env_.end(), 0.f);
  envPos_ = 0; envCount_ = 0; pend_ = 0; pendN_ = 0; onPos_ = 0; onCount_ = 0;
  valid_ = false; bpm_ = 0; conf_ = 0; pendBpm_ = 0; pendCount_ = 0;
}

void TempoEstimator::invalidate() { reset(); }

void TempoEstimator::pushNovelty(float e) {
  pend_ += e;
  if (++pendN_ < 2) return;
  env_[envPos_] = pend_ * 0.5f; pend_ = 0; pendN_ = 0;
  envPos_ = (envPos_ + 1 == lenv_) ? 0 : envPos_ + 1;
  ++envCount_;
}

void TempoEstimator::pushOnset(double t, float w) {
  onT_[onPos_] = t; onW_[onPos_] = w;
  onPos_ = (onPos_ + 1) % kOnsetCap;
  if (onCount_ < kOnsetCap) ++onCount_;
}

void TempoEstimator::decay() {
  conf_ *= 0.85f;
  if (conf_ < kValidConf * 0.6f) { valid_ = false; conf_ = 0; bpm_ = 0; }
}

bool TempoEstimator::update(double nowSec) {
  const int n = (int)std::min<uint64_t>(envCount_, (uint64_t)lenv_);
  if (n < (int)(4.0 * envRate_)) return false;
  // Gather onsets within the context window.
  double ot[kOnsetCap]; float ow[kOnsetCap]; int no = 0;
  for (int i = 0; i < onCount_; ++i) {
    const int idx = (onPos_ - 1 - i + kOnsetCap) % kOnsetCap;
    if (nowSec - onT_[idx] > kOnsetCtxSec) break;
    ot[no] = onT_[idx]; ow[no] = onW_[idx]; ++no;
  }
  if (no < 5) { if (valid_) decay(); return false; }

  // Linearize (oldest first) and remove mean.
  const int start = (envCount_ >= (uint64_t)lenv_) ? envPos_ : 0;
  double mean = 0;
  for (int i = 0; i < n; ++i) { lin_[i] = env_[(start + i) % lenv_]; mean += lin_[i]; }
  mean /= n;
  {                                                       // 5-tap binomial smoothing: makes the ACF robust to fractional-lag peaks
    float a1 = lin_[0], a2 = lin_[0];
    for (int i = 0; i < n; ++i) {
      const float x0 = lin_[i];
      const float p1 = i > 0 ? a1 : x0, p2 = i > 1 ? a2 : x0;
      const float n1 = i + 1 < n ? lin_[i + 1] : x0, n2 = i + 2 < n ? lin_[i + 2] : x0;
      a2 = a1; a1 = x0;
      lin2_[i] = (p2 + 4 * p1 + 6 * x0 + 4 * n1 + n2) * (1.0f / 16.0f);
    }
    for (int i = 0; i < n; ++i) lin_[i] = lin2_[i];
  }
  double e0 = 0;
  for (int i = 0; i < n; ++i) { lin_[i] -= (float)mean; e0 += (double)lin_[i] * lin_[i]; }
  if (e0 < 1e-9) { if (valid_) decay(); return false; }
  const int lagTop = std::min(maxLag_, n - 8);
  if (lagTop < lagMin_ + 2) return false;
  for (int L = 1; L <= lagTop; ++L) {
    double s = 0; const float* a = lin_.data(); const float* b = lin_.data() + L;
    for (int i = 0; i < n - L; ++i) s += (double)a[i] * b[i];
    const double v = s * ((double)n / (n - L)) / e0;           // unbiased, normalized by energy
    acf_[L] = (float)(v > 0 ? v : 0);
  }
  for (int L = 1; L < lagTop; ++L) sm_[L] = 0.25f * acf_[L - 1] + 0.5f * acf_[L] + 0.25f * acf_[L + 1];
  sm_[0] = sm_[lagTop] = 0;
  // Score candidate lags with a 2nd-harmonic comb and a mild log-tempo prior.
  const int lmax = std::min(lagMax_, lagTop - 1);
  double best = 0;
  double* sc = sc_.data(); int cnt = 0; const int firstL = lagMin_;
  for (int L = lagMin_; L <= lmax; ++L, ++cnt) {
    const float a = sm_[L];
    double s = 0;
    if (a >= sm_[L - 1] && a >= sm_[L + 1] && a > 0) {
      s = std::max(a, std::max(acf_[L - 1], acf_[L + 1]));
      if (2 * L + 2 <= lagTop && 2 * L <= (int)(0.6 * n))                  // tolerant 2nd-harmonic (lag quantization)
        s += 0.5 * std::max(acf_[2 * L - 1], std::max(acf_[2 * L], std::max(acf_[2 * L + 1], std::max(acf_[2 * L - 2], acf_[2 * L + 2]))));
      const double bpm = 60.0 * envRate_ / L;
      const double o = std::log(bpm / 120.0) / 0.6931471805599453 / 1.4;
      s *= std::exp(-0.5 * o * o);
    }
    sc[cnt] = s; if (s > best) best = s;
  }
  if (best <= 0.05) { if (valid_) decay(); return false; }
  if (getenv("TEMPO_DEBUG")) for (int i = 0; i < cnt; ++i) if (sc[i] > 0.3 * best) fprintf(stderr, "   L=%d s=%.3f acf=%.3f\n", firstL + i, sc[i], acf_[firstL + i]);
  int bestL = -1;
  for (int i = 0; i < cnt; ++i) if (sc[i] >= 0.8 * best) { bestL = firstL + i; break; }   // fastest near-tie
  if (bestL < 0) return false;
  double Lf = bestL;
  {
    const double a = sm_[bestL - 1], b = sm_[bestL], c = sm_[bestL + 1];
    const double d = a - 2 * b + c;
    if (d < 0) Lf += std::max(-0.5, std::min(0.5, 0.5 * (a - c) / d));
  }
  const double P0 = Lf / envRate_;

  // Refine: phase coherence of onset times over +-5% around P0.
  double tw = 0; for (int i = 0; i < no; ++i) tw += ow[i];
  if (tw <= 0) tw = 1;
  constexpr int kSteps = 21;
  double rr[kSteps]; int bi = 0;
  const double tp = 6.283185307179586;
  for (int j = 0; j < kSteps; ++j) {
    const double P = P0 * (1.0 + 0.005 * (j - 10));
    double sr = 0, si = 0;
    for (int i = 0; i < no; ++i) {
      const double ph = tp * std::fmod(ot[i], P) / P;
      sr += ow[i] * std::cos(ph); si += ow[i] * std::sin(ph);
    }
    rr[j] = std::sqrt(sr * sr + si * si) / tw;
    if (rr[j] > rr[bi]) bi = j;
  }
  double jf = bi;
  if (bi > 0 && bi < kSteps - 1) {
    const double a = rr[bi - 1], b = rr[bi], c = rr[bi + 1], d = a - 2 * b + c;
    if (d < 0) jf += std::max(-0.5, std::min(0.5, 0.5 * (a - c) / d));
  }
  const double P = P0 * (1.0 + 0.005 * (jf - 10));
  double bpm = 60.0 / P;
  while (bpm < kMinBpm) bpm *= 2;
  while (bpm > kMaxBpm) bpm /= 2;
  const float conf = (float)(rr[bi] * std::min(1.0, no / 8.0));
  if (getenv("TEMPO_DEBUG")) fprintf(stderr, "tempo upd t=%.2f no=%d bestL=%d P0=%.4f bpm0=%.2f r=%.3f bi=%d bpm=%.2f best=%.3f\n", nowSec, no, bestL, P0, 60/P0, rr[bi], bi, bpm, best);

  if (conf < kValidConf) { if (valid_) decay(); return false; }
  if (!valid_) {
    valid_ = true; bpm_ = bpm; conf_ = conf; pendCount_ = 0;
    return true;
  }
  if (std::fabs(bpm / bpm_ - 1.0) < 0.04) {
    bpm_ += 0.5 * (bpm - bpm_);
    conf_ += 0.5f * (conf - conf_);
    pendCount_ = 0;
    return true;
  }
  // Different tempo: require two consistent updates (or clearly higher confidence).
  if (pendCount_ > 0 && std::fabs(bpm / pendBpm_ - 1.0) < 0.04) ++pendCount_;
  else { pendBpm_ = bpm; pendCount_ = 1; }
  if (pendCount_ >= 2 || conf > conf_ + 0.25f) {
    bpm_ = bpm; conf_ = conf; pendCount_ = 0;
    return true;
  }
  return false;
}

} // namespace analysis
} // namespace ss
