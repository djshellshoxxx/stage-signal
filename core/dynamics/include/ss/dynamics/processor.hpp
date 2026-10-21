// MOD-03 dynamics: digital trim -> stereo-linked compressor -> lookahead sample-peak limiter -> telemetry.
// Spec section 12 "Recording/broadcast dynamics". Defaults and ranges are the section 12 table.
//
// Threading: prepare()/setParams()/resetHolds()/telemetry() may be called from any non-audio thread;
// process() is called by exactly one audio/DSP thread. process() never allocates, locks or does I/O.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "ss/base/error.hpp"

namespace ss::dynamics {

constexpr float kSilenceDb = -120.0f;
constexpr float kNearFullScaleDb = -0.5f;        // spec 12: input near-full-scale warning at >= -0.5 dBFS
constexpr std::uint32_t kPossibleClipRun = 3;    // consecutive near-full-scale samples labelled "possible input clipping"

// ---- parameters ---------------------------------------------------------------------------------
struct DynamicsParams {
    float trimDb = 0.0f;            // -24 .. +12
    float thresholdDb = -12.0f;     // -36 .. 0   (dBFS, applied to the trimmed max-channel peak)
    float ratio = 3.0f;             // 1 .. 10    (x:1)
    float attackMs = 10.0f;         // 0.5 .. 100 (one-pole time constant of the gain-reduction smoother)
    float releaseMs = 150.0f;       // 20 .. 2000
    float kneeDb = 6.0f;            // 0 .. 12
    float makeupDb = 0.0f;          // 0 .. 12    (auto makeup is off)
    float ceilingDb = -1.0f;        // -12 .. -0.5 (dBFS sample peak)
    float limiterReleaseMs = 100.0f;  // 20 .. 500
    bool bypass = false;            // equally delayed dry path, click-free ramp
};

enum ParamBit : std::uint32_t {
    kPTrim = 1u << 0, kPThreshold = 1u << 1, kPRatio = 1u << 2, kPAttack = 1u << 3, kPRelease = 1u << 4,
    kPKnee = 1u << 5, kPMakeup = 1u << 6, kPCeiling = 1u << 7, kPLimiterRelease = 1u << 8
};

// Out-of-range values are clamped (NaN/Inf -> default) and reported, never silently accepted.
struct ParamReport {
    std::uint32_t clampedMask = 0;
    bool anyClamped() const { return clampedMask != 0; }
    std::string describe() const;     // e.g. "ratio,ceiling" (empty when nothing was clamped)
};

DynamicsParams clampParams(const DynamicsParams& in, ParamReport* report);

// ---- structural configuration (needs allocation; applied via prepare()) ---------------------------
struct DynamicsConfig {
    std::uint32_t sampleRateHz = 48000;     // 8000 .. 384000
    std::uint32_t inChannels = 2;           // 1 or 2
    bool monoToStereo = false;              // explicit route: mono input duplicated to L/R (ignored for stereo)
    float lookaheadMs = 5.0f;               // 1 .. 20; fixed per admitted profile
    bool truePeakMeter = true;              // 4x oversampled meter on the output (meter only)
};

struct PrepareInfo {
    std::uint32_t sampleRateHz = 0;
    std::uint32_t inChannels = 0;
    std::uint32_t outChannels = 0;
    std::uint32_t processingDelaySamples = 0;   // equal for wet and dry paths
};

// ---- telemetry ---------------------------------------------------------------------------------
// Peak/GR "hold" fields are maxima since the last resetHolds(); *Current fields are the last block.
// Input fields are measured on the raw input (before trim and before the dynamics): REQ-A10.
// Output fields are measured after limiter/bypass mix: they describe the limiter-controlled signal.
struct DynamicsTelemetry {
    bool ready = false;
    bool bypassed = false;
    // raw input (pre-trim)
    float inputPeakDb = kSilenceDb;
    bool inputNearFullScale = false;          // some sample >= -0.5 dBFS since reset
    bool inputPossibleClipping = false;       // a run >= kPossibleClipRun near-full-scale samples since reset
    std::uint32_t inputClipRunMax = 0;
    std::uint64_t inputNearFullScaleSamples = 0;
    // processed output
    float outputPeakDb = kSilenceDb;          // sample peak
    float outputTruePeakDb = kSilenceDb;      // 4x meter estimate (kSilenceDb when disabled)
    std::uint64_t outputOverFullScaleSamples = 0;   // |y| >= 1.0 after processing
    // dynamics activity
    float compressorGrDb = 0.0f, compressorGrMaxDb = 0.0f;   // positive dB of gain reduction
    float limiterGrDb = 0.0f, limiterGrMaxDb = 0.0f;
    bool limiterActive = false;               // limiter reduced gain during the last block
    std::uint64_t limiterActiveSamples = 0;   // cumulative since reset
    // bookkeeping
    std::uint64_t sanitizedSamples = 0;       // NaN/Inf inputs replaced by 0
    std::uint64_t processedFrames = 0;
    std::uint32_t processingDelaySamples = 0;
};

class DynamicsProcessor {
public:
    DynamicsProcessor();
    ~DynamicsProcessor();
    DynamicsProcessor(const DynamicsProcessor&) = delete;
    DynamicsProcessor& operator=(const DynamicsProcessor&) = delete;

    // Control thread. Validates and builds a new engine (allocates), stages it; process() adopts it at the
    // next block boundary. State (delay lines, envelopes) of the new engine starts clean, so swap between
    // sessions or accept a one-time discontinuity. Rejects bad rate/channel counts before allocating.
    Result<PrepareInfo> prepare(const DynamicsConfig& cfg);

    // Any thread. Clamps and reports; applied smoothly (no zipper noise) by the audio thread.
    ParamReport setParams(const DynamicsParams& p);
    DynamicsParams params() const;

    // Audio thread. `in` holds `frames` interleaved frames of cfg.inChannels; `out` receives `frames` frames of
    // PrepareInfo::outChannels. in == out is allowed when the channel counts match. Returns false (and writes
    // nothing) until a prepared engine exists.
    bool process(const float* in, float* out, std::uint32_t frames);

    void resetHolds();                        // clears hold fields (applied at the next block)
    DynamicsTelemetry telemetry() const;      // relaxed snapshot; fields may be one block apart

    // Active engine description (valid after the first process() adopted the engine).
    PrepareInfo activeInfo() const;

    // REQ-R02: settings + activity as JSON text for recording metadata. Raw input warnings and the
    // limiter-controlled output are separate objects.
    std::string metadataJson() const;

private:
    struct Engine;
    struct Impl;
    Impl* impl_;
};

}  // namespace ss::dynamics
