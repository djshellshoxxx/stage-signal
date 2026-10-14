// MOD-14 soak helper (REQ-X06): detects unbounded growth in memory/queue series over a long run (e.g. 4 h) and rolls the
// soak criteria up into a pass/fail report. Series are downsampled into a fixed number of buckets, so memory stays bounded.
#pragma once
#include "core/types.h"
#include <string>
#include <vector>

namespace ss {

struct TrendResult {
  bool enough = false;                 // enough data to judge
  double slopePerHour = 0;             // least-squares slope (units/hour) after warm-up exclusion
  double r2 = 0;                       // linear fit quality
  double growthTotal = 0;              // slope * observed duration
  double firstThirdMedian = 0, lastThirdMedian = 0;
  bool unbounded = false;
};
struct TrendConfig {
  double warmupFraction = 0.10;        // leading fraction ignored
  double minDurationSec = 600;         // below this, refuse to judge
  double allowedGrowthTotal = 2.0 * 1048576.0;   // units (bytes): growth tolerated over the observed span
  double minR2 = 0.5;                  // monotone-ish requirement to call it a leak rather than noise
};

class SeriesTrend {
 public:
  explicit SeriesTrend(size_t maxBuckets = 512) : max_(maxBuckets < 16 ? 16 : maxBuckets) {}
  void add(Ns t, double v);            // bounded: merges adjacent buckets when full (keeps min/mean/max shape)
  size_t size() const { return pts_.size(); }
  TrendResult analyze(const TrendConfig& cfg = TrendConfig()) const;
  double maxValue() const { return maxV_; }
 private:
  struct P { double t, sum, n, mx; };
  void compact();
  size_t max_; std::vector<P> pts_; double maxV_ = 0; double bucketSpan_ = 0; double firstT_ = -1;
};

struct SoakInputs {
  double durationHours = 0; bool crashed = false; bool stuckBlackout = false; uint64_t undeclaredRecordingLossEvents = 0; uint64_t captureOverruns = 0;
  bool captureCounterAvailable = true;
};
struct SoakReport {
  bool pass = false, complete = true; std::vector<std::string> failures, unavailable;
  TrendResult workingSet, privateBytes; std::vector<std::pair<std::string, TrendResult>> queues;
};

class SoakMonitor {
 public:
  SoakMonitor(TrendConfig cfg = TrendConfig()) : cfg_(cfg) {}
  void sampleMemory(Ns t, double workingSetBytes, double privateBytes) { ws_.add(t, workingSetBytes); pv_.add(t, privateBytes); ++samples_; }
  void sampleQueue(const std::string& name, Ns t, size_t occupancy, size_t capacity);
  SoakReport finish(const SoakInputs& in, double requiredHours = 4.0) const;
  uint64_t samples() const { return samples_; }
 private:
  struct Q { std::string name; SeriesTrend trend; size_t cap = 0, maxOcc = 0; };
  TrendConfig cfg_; SeriesTrend ws_, pv_; std::vector<Q> qs_; uint64_t samples_ = 0;
};

} // namespace ss
