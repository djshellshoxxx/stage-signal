// MOD-03 audio analyzer (spec §12, IF-03). Consumes raw captured AudioBlocks (before dynamics) and
// publishes FeatureSnapshot. Single-threaded: process()/snapshot()/reset() must be called from the one
// analyzer worker thread. process() performs no allocation, locks or I/O after construction.
// Manual/tap tempo and manual key overrides are NOT handled here (the show engine owns them).
#pragma once
#include "core/types.h"
#include <memory>

namespace ss {

struct AnalyzerCounters {
  uint64_t blocks = 0;              // blocks accepted
  uint64_t frames = 0;              // sample frames analysed
  uint64_t sanitized = 0;           // NaN/Inf samples replaced by 0 (IF-01)
  uint64_t discontinuities = 0;     // AF_Discontinuity blocks handled
  uint64_t rejectedBlocks = 0;      // stale generation or rate/channel mismatch
  uint64_t fftFrames = 0;           // 1024/256 band+flux frames computed
};

class Analyzer {
 public:
  // rate: Hz (44100/48000 expected, any 8k..192k works); channels: 1 or 2 analysed (extra channels ignored).
  Analyzer(uint32_t rate, uint32_t channels);
  ~Analyzer();
  Analyzer(const Analyzer&) = delete;
  Analyzer& operator=(const Analyzer&) = delete;

  void process(const AudioBlock& block);
  // Latest features. nowNs is on the same clock as AudioBlock::captureNs (used for estimate ageNs).
  // snapshot().onset is true once per new onset since the previous snapshot call.
  FeatureSnapshot snapshot(Ns nowNs);
  void reset();                      // forget all state (device reopen / new session)
  const AnalyzerCounters& counters() const;
  uint32_t rate() const;
  uint32_t channels() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

} // namespace ss
