// MOD-03 recording/broadcast dynamics (spec §12, IF-04, REQ-R02): trim -> stereo-linked compressor ->
// sample-peak lookahead limiter, with click-free bypass through an equally delayed dry path.
// Threading: stageConfig() may be called from ONE non-audio thread at a time (serialize externally if
// several); process()/telemetry()/reset() belong to the single DSP worker. process() performs no heap
// allocation or locking (the AudioBlock overload only resizes `out` if its capacity is too small; pre-size it).
#pragma once
#include "core/types.h"
#include <atomic>
#include <memory>

namespace ss {

struct DynamicsConfig {
  bool bypass = false;            // true: equally delayed dry path (no trim/compressor/limiter) with a 10 ms ramp
  float trimDb = 0.f;             // -24..+12
  bool compressorOn = true;
  float thresholdDb = -12.f;      // -36..0 dBFS
  float ratio = 3.f;              // 1..10 (:1)
  float attackMs = 10.f;          // 0.5..100
  float releaseMs = 150.f;        // 20..2000
  float kneeDb = 6.f;             // 0..12
  float makeupDb = 0.f;           // 0..12
  bool limiterOn = true;
  float ceilingDb = -1.f;         // -12..-0.5 dBFS (sample peak)
  float limiterReleaseMs = 100.f; // 20..500
  // Returns a copy with every field clamped to its documented range (NaN -> default).
  DynamicsConfig clamped() const;
};

struct DynamicsTelemetry {
  float compGrDb = 0.f;           // current compressor gain reduction (>=0 dB)
  float limGrDb = 0.f;            // current limiter gain reduction (>=0 dB)
  float maxCompGrDb = 0.f, maxLimGrDb = 0.f;   // maxima since reset()
  float inPeakDb = kDbSilence;    // sample peak of input (pre-trim) since the previous telemetry() call
  float outPeakDb = kDbSilence;   // sample peak of output since the previous telemetry() call
  bool bypassed = false;          // bypass target state
  uint64_t configRevision = 0;    // revision of the config currently applied
  uint64_t frames = 0;            // frames processed since reset()
  uint64_t compActiveFrames = 0;  // frames with compressor GR > 0.1 dB (activity metadata, REQ-R02)
  uint64_t limActiveFrames = 0;   // frames with limiter GR > 0.1 dB
  uint64_t bypassedFrames = 0;
  uint64_t sanitized = 0;         // NaN/Inf input samples zeroed
  uint32_t delaySamples = 0;      // processing latency (lookahead), identical in bypass
};

class Dynamics {
 public:
  Dynamics(uint32_t rate, uint32_t channels);
  ~Dynamics();
  Dynamics(const Dynamics&) = delete;
  Dynamics& operator=(const Dynamics&) = delete;

  uint32_t delaySamples() const;                          // lookahead delay, fixed at construction (5 ms)
  // Off-thread: validate+clamp and publish a new configuration; returns its revision (>=1). Preallocated, lock-free.
  uint64_t stageConfig(const DynamicsConfig& cfg);
  const DynamicsConfig& activeConfig() const;             // config in effect (worker thread)

  // Process `frames` interleaved frames. in/out may alias exactly (in-place) but must not partially overlap.
  void process(const float* in, float* out, size_t frames);
  // Block API (IF-04): copies metadata, output block gets the same frame count; samples are delayed by delaySamples().
  bool process(const AudioBlock& in, AudioBlock& out);
  DynamicsTelemetry telemetry();                          // resets the per-interval peak holds
  void reset();                                           // clear all state (does not change the staged config)

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

} // namespace ss
