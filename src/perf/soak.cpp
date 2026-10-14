#include "perf/soak.h"
#include <algorithm>
#include <cmath>

namespace ss {

void SeriesTrend::add(Ns t, double v) {
  double ts = nsToSec(t); if (firstT_ < 0) { firstT_ = ts; bucketSpan_ = 0; }
  if (v > maxV_) maxV_ = v;
  if (!pts_.empty() && bucketSpan_ > 0 && ts - pts_.back().t < bucketSpan_) { P& b = pts_.back(); b.sum += v; b.n += 1; b.mx = std::max(b.mx, v); return; }
  pts_.push_back({ts, v, 1, v});
  if (pts_.size() > max_) compact();
}
void SeriesTrend::compact() {
  std::vector<P> n; for (size_t i = 0; i < pts_.size(); i += 2) {
    P a = pts_[i]; if (i + 1 < pts_.size()) { const P& b = pts_[i + 1]; a.sum += b.sum; a.n += b.n; a.mx = std::max(a.mx, b.mx); } n.push_back(a); }
  pts_.swap(n);
  double span = pts_.size() > 1 ? (pts_.back().t - pts_.front().t) / (double)(pts_.size() - 1) : 1.0;
  bucketSpan_ = std::max(bucketSpan_ * 2, span);
}
static double med(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

TrendResult SeriesTrend::analyze(const TrendConfig& c) const {
  TrendResult r; if (pts_.size() < 8) return r;
  double t0 = pts_.front().t, t1 = pts_.back().t; if (t1 - t0 < c.minDurationSec) return r;
  size_t skip = (size_t)((double)pts_.size() * c.warmupFraction); std::vector<std::pair<double, double>> v;
  for (size_t i = skip; i < pts_.size(); ++i) v.push_back({pts_[i].t, pts_[i].sum / pts_[i].n});
  if (v.size() < 6) return r;
  double n = (double)v.size(), sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, tb = v.front().first;
  for (auto& p : v) { double x = p.first - tb, y = p.second; sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; }
  double den = n * sxx - sx * sx; if (den <= 0) return r;
  double slope = (n * sxy - sx * sy) / den; double vy = n * syy - sy * sy;
  r.r2 = vy > 0 ? (n * sxy - sx * sy) * (n * sxy - sx * sy) / (den * vy) : 0;
  r.slopePerHour = slope * 3600.0; r.growthTotal = slope * (v.back().first - tb);
  size_t th = v.size() / 3; std::vector<double> a, b; for (size_t i = 0; i < th; ++i) a.push_back(v[i].second); for (size_t i = v.size() - th; i < v.size(); ++i) b.push_back(v[i].second);
  r.firstThirdMedian = med(a); r.lastThirdMedian = med(b); r.enough = true;
  // unbounded growth: significant fitted growth with a monotone-ish trend AND the late plateau really is higher than the early one
  r.unbounded = r.growthTotal > c.allowedGrowthTotal && r.r2 >= c.minR2 && (r.lastThirdMedian - r.firstThirdMedian) > c.allowedGrowthTotal * 0.5;
  return r;
}

void SoakMonitor::sampleQueue(const std::string& name, Ns t, size_t occ, size_t cap) {
  for (auto& q : qs_) if (q.name == name) { q.trend.add(t, (double)occ); q.cap = cap; q.maxOcc = std::max(q.maxOcc, occ); return; }
  if (qs_.size() >= 16) return;
  Q q; q.name = name.substr(0, 48); q.cap = cap; q.maxOcc = occ; q.trend.add(t, (double)occ); qs_.push_back(q);
}

SoakReport SoakMonitor::finish(const SoakInputs& in, double requiredHours) const {
  SoakReport r; r.pass = true;
  auto fail = [&](const std::string& s) { r.failures.push_back(s); r.pass = false; };
  if (in.durationHours < requiredHours) fail("soak shorter than required duration");
  if (in.crashed) fail("crash during soak");
  if (in.stuckBlackout) fail("stuck blackout");
  if (in.undeclaredRecordingLossEvents) fail("undeclared recording loss");
  if (!in.captureCounterAvailable) { r.unavailable.push_back("capture overrun counter"); r.complete = false; }
  else if (in.captureOverruns) fail("capture overruns during soak (must be zero)");
  r.workingSet = ws_.analyze(cfg_); r.privateBytes = pv_.analyze(cfg_);
  if (!r.workingSet.enough || !r.privateBytes.enough) { r.unavailable.push_back("insufficient memory samples to judge growth"); r.complete = false; }
  if (r.workingSet.unbounded) fail("working set grows without bound");
  if (r.privateBytes.unbounded) fail("private bytes grow without bound");
  TrendConfig qc = cfg_; qc.allowedGrowthTotal = 4;     // queue occupancy: more than a few slots of sustained growth is a problem
  for (auto& q : qs_) {
    TrendResult t = q.trend.analyze(qc); r.queues.push_back({q.name, t});
    if (q.cap && q.maxOcc > q.cap) fail("queue '" + q.name + "' exceeded capacity");
    if (t.unbounded) fail("queue '" + q.name + "' occupancy grows without bound");
  }
  if (!r.complete) r.pass = false;       // unverified is not a pass
  return r;
}

} // namespace ss
