#include "recording/sync.h"
#include <cmath>
#include <algorithm>
namespace ss {
using rec::samplesToNs;

void SyncTimeline::anchor(Ns firstCaptureNs, uint32_t rate, const DelayCompensation& c) {
  rate_ = rate; c_ = c; anchored_ = true;
  origin_ = firstCaptureNs - samplesToNs(c.audioProcessingDelaySamples, rate) - c.audioEncoderPrimingNs;
}

int64_t rescale(int64_t v, Rational from, Rational to) {
  // v * from.num/from.den / (to.num/to.den) = v * from.num * to.den / (from.den * to.num), rounded to nearest, split to avoid overflow
  if (!from.den || !to.num) return 0;
  long double r = (long double)v * (long double)from.num * (long double)to.den / ((long double)from.den * (long double)to.num);
  return (int64_t)(r >= 0 ? std::floor(r + 0.5L) : std::ceil(r - 0.5L));
}

void FrameCadence::configure(int n, int d) { num_ = n > 0 ? n : 30; den_ = d > 0 ? d : 1; }
void FrameCadence::reset(Ns s) { start_ = s; last_ = -1; first_ = false; dups_ = 0; drops_ = 0; }
Ns FrameCadence::slotToNs(int64_t slot) const { return (Ns)((long double)slot * den_ * 1e9L / num_ + 0.5L); }
FrameCadence::Slot FrameCadence::push(Ns pts) {
  Slot s;
  long double rel = (long double)(pts - start_) * num_ / ((long double)den_ * 1e9L);
  int64_t k = (int64_t)std::floor(rel + 0.5L);
  if (k < 0) k = 0;
  if (!first_) { first_ = true; last_ = k; s.slot = k; return s; }
  if (k <= last_) { s.drop = true; s.slot = last_; ++drops_; return s; }
  s.dupBefore = (uint32_t)(k - last_ - 1); dups_ += s.dupBefore; last_ = k; s.slot = k; return s;
}

// ---- DriftCorrector ----
Status DriftCorrector::configure(uint32_t rate, uint32_t ch, const DriftConfig& c) {
  if (rate < 8000 || ch < 1 || ch > 8) return Status::Fail(Err::InvalidArgument, "bad drift corrector format");
  rate_ = rate; ch_ = ch; cfg_ = c; if (cfg_.maxPpm > kMaxDriftPpm) cfg_.maxPpm = kMaxDriftPpm;
  reset(); return Status::Ok();
}
void DriftCorrector::reset() {
  S0_ = Sx_ = Sy_ = Sxx_ = Sxy_ = 0; haveAnchor_ = false; slope_ = 1.0; haveFit_ = false; offsetEwma_ = 0; haveOffset_ = false;
  hist_.clear(); pos_ = 0; primed_ = false; st_ = DriftStats();
}
void DriftCorrector::observe(double x, double y) {
  if (!haveAnchor_) { haveAnchor_ = true; x0_ = x; y0_ = y; firstX_ = lastX_ = 0; }
  double xr = x - x0_, yr = y - y0_;
  if (haveFit_) {   // outlier rejection against the current fit (scheduling hiccup, not clock behaviour)
    double xbar = Sx_ / S0_, ybar = Sy_ / S0_;
    double pred = ybar + slope_ * (xr - xbar);
    if (std::fabs(yr - pred) > 0.030) { ++st_.outliersIgnored; return; }
  }
  double dx = xr - lastX_; if (dx < 0) dx = 0;
  double lam = std::exp(-dx / cfg_.decaySec);
  S0_ *= lam; Sx_ *= lam; Sy_ *= lam; Sxx_ *= lam; Sxy_ *= lam;
  S0_ += 1; Sx_ += xr; Sy_ += yr; Sxx_ += xr * xr; Sxy_ += xr * yr; lastX_ = xr;
  if (xr - firstX_ >= cfg_.warmupSec && S0_ > 50) {
    double den = S0_ * Sxx_ - Sx_ * Sx_;
    if (den > 1e-9) { slope_ = (S0_ * Sxy_ - Sx_ * Sy_) / den; haveFit_ = true; }
  }
}

Status DriftCorrector::process(const float* in, size_t frames, Ns captureNs, bool tsValid, std::vector<float>& out) {
  out.clear();
  Status result = Status::Ok();
  if (!frames) return result;
  double xStart = (double)st_.inFrames / rate_;
  if (tsValid && !st_.excessive) {
    double yHost = (double)captureNs * 1e-9;
    observe(xStart, yHost);
    if (haveFit_) {
      double ppm = (1.0 / slope_ - 1.0) * 1e6;
      st_.measuredPpm = ppm; st_.converged = true;
      if (std::fabs(ppm) > cfg_.maxPpm) {
        st_.excessive = true; st_.appliedPpm = 0;
        result = Status::Fail(Err::TimestampInvalid, "audio/host clock mismatch " + std::to_string((int)ppm) + " ppm exceeds the +-" + std::to_string((int)cfg_.maxPpm) + " ppm correction bound; check the capture device clock/driver", false);
      }
    }
    // phase offset: output audio position vs host elapsed (since the first observation)
    double outSec = (double)st_.outFrames / rate_ - x0_;
    double hostElapsed = yHost - y0_;
    double e = outSec - hostElapsed;
    if (!haveOffset_) { offsetEwma_ = e; haveOffset_ = true; } else offsetEwma_ += 0.01 * (e - offsetEwma_);
    st_.offsetNs = offsetEwma_ * 1e9;
  }
  st_.inFrames += frames;
  if (!cfg_.enabled || st_.excessive || !st_.converged) {
    // passthrough (no estimate yet / disabled / error): keep resampler idle and consistent
    out.assign(in, in + frames * ch_);
    st_.outFrames += frames;
    st_.appliedPpm = 0;
    return result;
  }
  double applied = st_.measuredPpm + (offsetEwma_ / cfg_.slewSec) * 1e6;
  if (applied > cfg_.maxPpm) applied = cfg_.maxPpm; else if (applied < -cfg_.maxPpm) applied = -cfg_.maxPpm;
  st_.appliedPpm = applied;
  // Cubic (Catmull-Rom) read at step = 1 + applied/1e6 input frames per output frame. (audio fast => step>1 => fewer outputs)
  const double step = 1.0 + applied * 1e-6;
  if (!primed_) {   // first resampled block: seed history with 1 leading copy so interpolation has a left neighbour
    hist_.assign(in, in + ch_); pos_ = 1.0; primed_ = true;   // pos_ indexes frame 1 == first real frame after the seed copy
  }
  hist_.insert(hist_.end(), in, in + frames * ch_);
  size_t have = hist_.size() / ch_;
  out.reserve(frames * ch_ + ch_);
  while (true) {
    size_t i1 = (size_t)pos_;
    if (i1 + 2 >= have) break;
    double t = pos_ - (double)i1;
    for (uint32_t c = 0; c < ch_; ++c) {
      float y0 = hist_[(i1 - 1) * ch_ + c], y1 = hist_[i1 * ch_ + c], y2 = hist_[(i1 + 1) * ch_ + c], y3 = hist_[(i1 + 2) * ch_ + c];
      double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3, b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, cc = -0.5 * y0 + 0.5 * y2;
      out.push_back((float)(((a * t + b) * t + cc) * t + y1));
    }
    pos_ += step;
  }
  // discard consumed history, keep 1 frame left of pos_
  size_t keepFrom = (size_t)pos_ >= 1 ? (size_t)pos_ - 1 : 0;
  if (keepFrom > 0) { hist_.erase(hist_.begin(), hist_.begin() + keepFrom * ch_); pos_ -= (double)keepFrom; }
  st_.outFrames += out.size() / ch_;
  return result;
}

// ---- helpers ----
double meanLuma(const Frame& f) {
  if (f.px.empty()) return 0;
  double s = 0; size_t step = f.px.size() > 4096 ? f.px.size() / 4096 : 1; size_t n = 0;
  for (size_t i = 0; i < f.px.size(); i += step) { uint32_t p = f.px[i]; s += 0.299 * ((p >> 16) & 255) + 0.587 * ((p >> 8) & 255) + 0.114 * (p & 255); ++n; }
  return s / (double)n;
}
int detectFlashIndex(const std::vector<double>& l, double jump) {
  if (l.size() < 4) return -1;
  std::vector<double> b(l.begin(), l.begin() + 3); std::sort(b.begin(), b.end());
  double base = b[1];
  for (size_t i = 0; i < l.size(); ++i) if (l[i] >= base + jump) return (int)i;
  return -1;
}
int64_t detectClickSample(const float* x, size_t frames, uint32_t ch, float th) {
  for (size_t i = 0; i < frames; ++i) for (uint32_t c = 0; c < ch; ++c) if (std::fabs(x[i * ch + c]) >= th) return (int64_t)i;
  return -1;
}
std::vector<int64_t> detectClicks(const float* x, size_t frames, uint32_t ch, float th, size_t refr) {
  std::vector<int64_t> v; size_t i = 0;
  while (i < frames) {
    bool hit = false; for (uint32_t c = 0; c < ch; ++c) if (std::fabs(x[i * ch + c]) >= th) hit = true;
    if (hit) { v.push_back((int64_t)i); i += refr ? refr : 1; } else ++i;
  }
  return v;
}
double offsetDriftNsPerHour(const std::vector<double>& t, const std::vector<double>& o) {
  size_t n = std::min(t.size(), o.size()); if (n < 2) return 0;
  double st = 0, so = 0, stt = 0, sto = 0;
  for (size_t i = 0; i < n; ++i) { st += t[i]; so += o[i]; stt += t[i] * t[i]; sto += t[i] * o[i]; }
  double den = n * stt - st * st; if (std::fabs(den) < 1e-12) return 0;
  return (n * sto - st * so) / den * 3600.0;
}

} // namespace ss
