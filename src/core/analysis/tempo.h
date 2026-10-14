// Tempo estimator (MOD-03): novelty-curve autocorrelation (coarse, octave handling) refined by
// onset-time phase coherence. Cheap, decoupled: update() is called ~1 Hz by the Analyzer.
#pragma once
#include <cstdint>
#include <vector>

namespace ss {
namespace analysis {

class TempoEstimator {
 public:
  static constexpr double kMinBpm = 40.0, kMaxBpm = 240.0;
  explicit TempoEstimator(double frameRateHz);   // novelty frames per second
  void reset();
  void pushNovelty(float e);                     // one value per flux frame (>=0)
  void pushOnset(double tSec, float strength);   // detected onset time on the stream clock
  // Re-estimate; returns true if the estimate was refreshed. nowSec = stream clock.
  bool update(double nowSec);
  void invalidate();                             // silence: becomes Unknown, history cleared
  bool valid() const { return valid_; }
  double bpm() const { return bpm_; }
  float confidence() const { return conf_; }
 private:
  void decay();
  double fr_, envRate_;
  int lenv_, lagMin_, lagMax_, maxLag_;
  std::vector<float> env_, lin_, acf_, sm_, lin2_; std::vector<double> sc_;
  int envPos_ = 0; uint64_t envCount_ = 0; float pend_ = 0; int pendN_ = 0;
  std::vector<double> onT_; std::vector<float> onW_; int onPos_ = 0; int onCount_ = 0;
  bool valid_ = false; double bpm_ = 0; float conf_ = 0;
  double pendBpm_ = 0; int pendCount_ = 0;
};

} // namespace analysis
} // namespace ss
