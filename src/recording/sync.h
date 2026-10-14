// MOD-11 clock model (spec §12 "Clock model", REQ-R03). Portable, no OS clocks.
//  * Audio is sample-indexed; each AudioBlock anchors (firstSample, captureNs). Sample timestamps are NEVER replaced by
//    wall time or arrival time.
//  * Session timeline origin = host time of the first captured input sample minus the known audio processing delay
//    (dynamics lookahead + filter group delay) minus encoder priming; video host-clock PTS maps onto that timeline, so the
//    delayed processed audio and the video line up (compensation, not guesswork).
//  * Drift: measured audio-clock rate error vs host clock (decayed least-squares over block anchors) is removed by gentle
//    cubic resampling bounded to +-200 ppm; a measured mismatch beyond the bound is an ERROR (Err::TimestampInvalid), not
//    silently clamped. A slow phase-slew term removes accumulated offset (e.g. warm-up) without exceeding the bound.
#pragma once
#include "core/types.h"
#include "recording/bytes.h"

namespace ss {

constexpr double kMaxDriftPpm = 200.0;

struct Rational { int64_t num = 1, den = 1; };   // time base: seconds = num/den ticks

// ---- Timeline ------------------------------------------------------------------------------------------
struct DelayCompensation {
  uint32_t audioProcessingDelaySamples = 0;      // dynamics lookahead + filter group delay (IF-04 "sample delay")
  Ns audioEncoderPrimingNs = 0;                  // 0 for PCM; e.g. AAC priming if ever encoded
  Ns videoEncoderDelayNs = 0;                    // fixed latency of video encoder if packets are stamped late (MJPEG: 0)
  Ns cameraOffsetNs = 0;                         // user calibration for cameras with poor driver timestamps
};
class SyncTimeline {
 public:
  // Anchors the timeline on the first audio block (captureNs of its first input sample).
  void anchor(Ns firstCaptureNs, uint32_t rate, const DelayCompensation& c);
  bool anchored() const { return anchored_; }
  Ns originHostNs() const { return origin_; }
  // Video source PTS (host ns) -> session timeline ns (>= 0 clamp is the caller's choice; value may be negative).
  Ns videoToTimeline(Ns hostPtsNs) const { return hostPtsNs - c_.cameraOffsetNs - c_.videoEncoderDelayNs - origin_; }
  // Processed audio sample index (0 = first processed output sample) -> timeline ns.
  Ns audioToTimeline(uint64_t sample) const { return rec::samplesToNs(sample, rate_); }
  Ns hostToTimeline(Ns hostNs) const { return hostNs - origin_; }
  Ns timelineToHost(Ns t) const { return t + origin_; }
  const DelayCompensation& compensation() const { return c_; }
  Ns totalAudioDelayNs() const { return rec::samplesToNs(c_.audioProcessingDelaySamples, rate_) + c_.audioEncoderPrimingNs; }
 private:
  bool anchored_ = false; Ns origin_ = 0; uint32_t rate_ = 48000; DelayCompensation c_;
};

// Rescale a tick count between rational timebases with round-to-nearest (no float).
int64_t rescale(int64_t v, Rational from, Rational to);
inline int64_t nsToTicks(Ns ns, Rational tb) { return rescale(ns, Rational{1, 1000000000}, tb); }
inline Ns ticksToNs(int64_t t, Rational tb) { return rescale(t, tb, Rational{1, 1000000000}); }

// ---- Fixed-FPS cadence with duplicate/drop accounting ----------------------------------------------------
class FrameCadence {
 public:
  void configure(int fpsNum, int fpsDen);
  void reset(Ns startNs);                         // new segment: slot 0 is at startNs
  struct Slot { bool drop = false; int64_t slot = 0; uint32_t dupBefore = 0; };
  // Assigns a frame (timeline pts) to a fixed-rate slot. dupBefore = number of repeats of the PREVIOUS frame needed to
  // fill skipped slots; drop = another frame already owns that slot (rate > output fps).
  Slot push(Ns ptsNs);
  Ns slotToNs(int64_t slot) const;
  uint64_t duplicated() const { return dups_; }
  uint64_t dropped() const { return drops_; }
  int64_t lastSlot() const { return last_; }
  bool hasFrame() const { return last_ >= 0 || first_; }
 private:
  int num_ = 30, den_ = 1; Ns start_ = 0; int64_t last_ = -1; bool first_ = false; uint64_t dups_ = 0, drops_ = 0;
};

// ---- Drift measurement + correction ---------------------------------------------------------------------
struct DriftConfig {
  bool enabled = true;
  double maxPpm = kMaxDriftPpm;
  double warmupSec = 20.0;                        // no correction until the fit spans this much audio time
  double decaySec = 900.0;                        // forgetting horizon of the rate estimate
  double slewSec = 60.0;                          // time constant to remove accumulated phase offset
};
struct DriftStats {
  double measuredPpm = 0;                         // audio clock fast (+) / slow (-) relative to host clock
  double appliedPpm = 0;                          // correction currently applied (same sign convention)
  double offsetNs = 0;                            // smoothed (output audio position - host elapsed)
  uint64_t inFrames = 0, outFrames = 0;
  bool converged = false; bool excessive = false;
  uint64_t outliersIgnored = 0;
};
class DriftCorrector {
 public:
  Status configure(uint32_t rate, uint32_t channels, const DriftConfig& cfg = DriftConfig());
  // Feed contiguous input frames; appends corrected frames to `out` (cleared first). captureNs valid only when tsValid.
  // On |measured| > maxPpm returns Err::TimestampInvalid and from then on passes audio through unmodified (stats.excessive).
  Status process(const float* in, size_t frames, Ns captureNs, bool tsValid, std::vector<float>& out);
  const DriftStats& stats() const { return st_; }
  void reset();
 private:
  void observe(double x, double y);
  uint32_t rate_ = 48000, ch_ = 2; DriftConfig cfg_;
  // regression state
  double S0_ = 0, Sx_ = 0, Sy_ = 0, Sxx_ = 0, Sxy_ = 0, x0_ = 0, y0_ = 0, lastX_ = 0, firstX_ = 0; bool haveAnchor_ = false;
  double slope_ = 1.0; bool haveFit_ = false;
  double offsetEwma_ = 0; bool haveOffset_ = false;
  // resampler state
  std::vector<float> hist_;                       // pending input frames (interleaved) not yet fully consumed
  double pos_ = 0;                                // fractional read position within hist_ (frames)
  bool primed_ = false;
  DriftStats st_;
};

// ---- Flash/click offset helpers (sync acceptance tests, §12) ---------------------------------------------
double meanLuma(const Frame& f);                  // 0..255 (Rec.601-ish on 0x00RRGGBB)
// First index in `luma` whose value rises >= `jump` above the running baseline (median of the first 3), or -1.
int detectFlashIndex(const std::vector<double>& luma, double jump);
// First sample frame index where |x| (max over channels) >= threshold, or -1.
int64_t detectClickSample(const float* interleaved, size_t frames, uint32_t channels, float threshold);
// All onsets (rising through threshold with refractory) so periodic flash/click trains can be compared.
std::vector<int64_t> detectClicks(const float* interleaved, size_t frames, uint32_t channels, float threshold, size_t refractoryFrames);
// Offset = video event time - audio event time (positive => picture LATE vs sound). Times on the session timeline.
inline Ns flashClickOffsetNs(Ns flashTimelineNs, Ns clickTimelineNs) { return flashTimelineNs - clickTimelineNs; }
// Linear drift (ns/hour) of the offset series (offsets measured at times t[i]); least squares.
double offsetDriftNsPerHour(const std::vector<double>& tSec, const std::vector<double>& offsetNs);

} // namespace ss
