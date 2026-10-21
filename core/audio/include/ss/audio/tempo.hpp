// StageSignal audio: tempo estimation (REQ-A05) and tap/manual tempo control (REQ-A06).
// Spec: docs/specs/audio.md "Tempo".
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"

namespace ss::audio {

// Tempo search range and tunables (documented in the spec; provisional until a music corpus is measured).
struct TempoLimits {
    static constexpr double kMinBpm = 40.0, kMaxBpm = 240.0;     // search range (spec section 12)
    static constexpr double kPreferredMaxBpm = 200.0;            // octave resolution never prefers a faster tempo
    static constexpr double kWindowS = 8.0;                      // autocorrelation window
    static constexpr double kMinEvidenceS = 5.0;                 // first estimate needs this much envelope
    static constexpr double kUpdateS = 0.5;                      // new measurement period
    static constexpr double kSilenceHoldS = 1.5;                 // silence this long => Unknown, history cleared
    static constexpr double kStaleS = 8.0;                       // no accepted measurement this long => Unknown
    static constexpr double kMinEnvelopePeak = 0.05;             // onset envelope must reach this (flux units) in the window
    static constexpr double kConfR0 = 0.20, kConfSpan = 0.50;    // confidence = clamp((r - R0) / Span), r = normalised autocorrelation
    static constexpr double kMinConfidence = 0.30;               // measurements below this are rejected
    static constexpr double kAgreeRatio = 0.03;                  // |a/b-1| within this = same tempo
    static constexpr int kSwitchCount = 3;                       // consecutive agreeing candidates to change tempo
    static constexpr double kOctaveAccept = 0.80;                // sub-multiple peak must reach this fraction of r0
};

// Fed with one onset-strength value per analysis hop (e.g. positive spectral flux). Real-time safe after
// prepare(): fixed buffers, bounded work per push (autocorrelation is amortised over the update period).
class TempoEstimator {
public:
    bool prepare(std::uint32_t rateHz, std::uint32_t hopSamples);
    void reset();
    void push(float onsetStrength, bool silent);
    // Latest estimate. Source Unknown (bpm 0, confidence 0) when none; never a numeric-zero "valid" tempo.
    TempoState state() const;
    std::uint64_t frames() const { return frame_; }

private:
    Ns frameNs(std::uint64_t f) const { return samplesToNs(f * hop_, rate_); }
    void startCycle();
    void stepCycle();
    void finalise();
    void clearEstimate();
    double lagToBpm(double lag) const { return 60.0 * frameRate_ / lag; }

    std::uint32_t rate_ = 0, hop_ = 0;
    double frameRate_ = 0.0, meanA_ = 0.0, runMean_ = 0.0;
    std::size_t W_ = 0, minFrames_ = 0, updateFrames_ = 0, silenceFrames_ = 0, staleFrames_ = 0;
    std::size_t lagLo_ = 0, lagHi_ = 0, perFrame_ = 1;
    std::vector<float> buf_, work_, r_;
    std::size_t w_ = 0, filled_ = 0, cursor_ = 0, n_ = 0;
    double var_ = 0.0;
    bool cycling_ = false;
    std::uint64_t frame_ = 0, sinceCycle_ = 0, silentRun_ = 0;
    // estimate state
    bool valid_ = false;
    double bpm_ = 0.0, conf_ = 0.0;
    std::uint64_t lastUpdateFrame_ = 0, lastGoodFrame_ = 0;
    double candBpm_ = 0.0, candConf_ = 0.0;
    int candCount_ = 0;
    double pendBpm_ = 0.0; bool pend_ = false;     // first-estimate agreement
};

// ---- Tap / manual override (REQ-A06) ----------------------------------------------------------------
struct TempoControllerConfig {
    double minBpm = 30.0, maxBpm = 300.0;       // accepted manual / tap range
    Ns tapResetGap = msToNs(2500);              // a gap longer than this starts a new tap sequence
    int maxTaps = 8;                            // taps averaged (the most recent maxTaps; maxTaps-1 intervals)
};

// Deterministic: the only inputs are the calls and the caller supplied timestamps (no clock inside).
class TempoController {
public:
    explicit TempoController(TempoControllerConfig cfg = {});
    // Register a tap at nowNs (monotonic). Returns true once the tap sequence has produced a tempo
    // (>= 2 taps with an in-range average); that sets source Tap.
    bool tap(Ns nowNs);
    // Manual BPM; NaN/inf/out-of-range rejected with InvalidArgument and the previous state kept.
    Status setManual(double bpm, Ns nowNs);
    // Return to the estimated tempo. Idempotent.
    void release();
    // Combine with the latest detector estimate. Estimated while no override; Tap/Manual otherwise
    // (confidence 1, age since the override was set/updated).
    TempoState resolve(const TempoState& estimate, Ns nowNs) const;
    ValueSource overrideSource() const { return src_; }       // Unknown = following estimates
    int tapCount() const { return nTaps_; }

private:
    TempoControllerConfig cfg_;
    std::array<Ns, 16> taps_{};
    int nTaps_ = 0;
    ValueSource src_ = ValueSource::Unknown;
    double bpm_ = 0.0;
    Ns setNs_ = 0;
};

}  // namespace ss::audio
