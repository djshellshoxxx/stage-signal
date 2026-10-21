// StageSignal audio: MOD-03 analysis front end. Consumes IF-01 AudioBlockView, produces IF-03 FeatureSnapshot.
// Spec: docs/specs/audio.md. Threading: one analysis thread calls prepare/process/snapshot/copy*; publish the
// snapshot to other threads through a mailbox/queue. process() allocates nothing and takes no locks.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ss/audio/energy_arc.hpp"
#include "ss/audio/fft.hpp"
#include "ss/audio/key.hpp"
#include "ss/audio/tempo.hpp"
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"

namespace ss::audio {

struct AnalyzerConstants {
    static constexpr std::size_t kFrame = 1024, kHop = 256;                  // spectral analysis (spec section 12)
    static constexpr std::size_t kSpectrumBins = 32, kWaveformPoints = 256;
    static constexpr double kBandEdgesHz[4] = {30.0, 250.0, 2000.0, 12000.0};
    static constexpr double kSpectrumLoHz = 30.0, kSpectrumHiHz = 16000.0;
    static constexpr double kSpectrumFloorDb = -70.0;                        // 0.0 on the 0..1 scale
    static constexpr float kSpectrumRelease = 0.92f;                         // per hop (~64 ms time constant at 48 kHz)
    static constexpr double kNearFullScaleDb = -0.5;                         // "possible input clipping" (>= -0.5 dBFS)
    static constexpr double kSilenceDb = -70.0;                              // window RMS below this (all channels) = silent
    static constexpr float kSanitizeLimit = 64.f;                            // |x| above this is clamped (and counted)
    static constexpr std::size_t kSubWindows = 10;                           // 10 x 5 ms = 50 ms rolling window
    static constexpr float kFluxCompression = 1000.f;                        // log(1 + k * amplitude) magnitude compression
    static constexpr float kFluxScale = 0.5f;                                // strength = 1 - exp(-flux / kFluxScale)
    static constexpr std::size_t kFluxBands = 24;                            // log-spaced flux bands
    static constexpr std::size_t kFluxHistory = 64;                          // adaptive-threshold history (~0.35 s)
    static constexpr float kThresholdStd = 3.5f;                             // threshold = mean + kThresholdStd * std
    static constexpr float kThresholdFloor = 0.06f;                          // absolute minimum flux for an onset
    static constexpr float kPostOnsetRatio = 0.6f;                           // threshold floor after an onset = ratio * its flux ...
    static constexpr double kPostOnsetTauS = 0.25;                           // ... decaying with this time constant
    static constexpr double kRefractoryMs = 100.0;                           // spec: >= 100 ms
    static constexpr std::size_t kOnsetQueue = 128;
};

struct OnsetEvent {
    Ns timeNs = 0;          // estimated onset time on the snapshot timeline (see sourceNs)
    float strength = 0.f;   // 0..1
};

struct AnalyzerStats {
    std::uint64_t blocks = 0, samples = 0;
    std::uint64_t nonFiniteSamples = 0;      // NaN/Inf replaced by 0
    std::uint64_t clampedSamples = 0;        // |x| > kSanitizeLimit
    std::uint64_t discontinuities = 0;       // blocks flagged kAudioDiscontinuity
    std::uint64_t mismatchedBlocks = 0;      // rate/channel count differs from prepare(); block ignored
    std::uint64_t nearFullScaleSamples = 0;
    std::uint64_t onsets = 0, onsetsDropped = 0;   // dropped = event ring was full (oldest overwritten)
};

class Analyzer {
public:
    Analyzer() = default;
    // rateHz 8000..192000 (44100/48000 qualified), channels 1 or 2. Allocates; call off the real-time path.
    Status prepare(std::uint32_t rateHz, std::uint32_t channels);
    void reset();                         // clear all state, keep configuration (no allocation)
    bool prepared() const { return ready_; }

    // Real-time safe. Any frame count (including 1), NaN/Inf tolerant, blocks whose rate/channels differ from
    // prepare() are ignored and counted.
    void process(const AudioBlockView& block);

    // Latest features. snapshot() consumes the onset latch (onsetFired is true in the first snapshot taken after
    // an onset was detected, even when many blocks were processed in between); peek() does not.
    FeatureSnapshot snapshot();
    FeatureSnapshot peek() const { return build(onsetLatched_); }

    // 32 log-spaced bins 30 Hz..16 kHz, 0..1: value = clamp((10 log10(power) + 70) / 70) where power is the total
    // FFT power in the bin in band units (a sine of amplitude A reads about 20 log10 A, up to 1.8 dB low when it
    // falls between FFT bins; a display bin narrower than one FFT bin interpolates). Instant attack, release 0.92/hop.
    // Bins above Nyquist are 0. Energies of the two channels are averaged per FFT bin (no phase cancellation).
    void copySpectrum(float* out32) const;
    // 256 samples (-1..1) of the newest 1024 mono-mixed input samples (groups of 4 averaged), oldest first.
    void copyWaveform(float* out256) const;
    // Pops detected onsets (oldest first). Returns the number written.
    std::size_t drainOnsets(OnsetEvent* out, std::size_t cap);

    const AnalyzerStats& stats() const { return stats_; }
    Status setEnergyArcParams(const EnergyArcParams& p) { return arc_.setParams(p); }
    const EnergyArcParams& energyArcParams() const { return arc_.params(); }
    std::uint32_t rateHz() const { return rate_; }
    std::uint32_t channels() const { return ch_; }

private:
    struct Sub { double sumsq[2] = {0, 0}; float peak[2] = {0, 0}; std::uint32_t maxRun = 0; };
    FeatureSnapshot build(bool fired) const;
    void flushDiscontinuity(const AudioBlockView& b);
    void pushFrame(const float* x);
    void finishSub();
    void runFrame();
    Ns toNs(std::uint64_t sample) const { return rate_ ? timeBaseNs_ + samplesToNs(sample, rate_) : 0; }

    bool ready_ = false;
    std::uint32_t rate_ = 0, ch_ = 0;
    std::uint32_t subLen_ = 0, subCount_ = 0, subIdx_ = 0, subFilled_ = 0;
    std::array<Sub, AnalyzerConstants::kSubWindows> subs_{};
    Sub cur_;
    // published meters
    float rmsDb_[2] = {-120.f, -120.f}, peakDb_[2] = {-120.f, -120.f};
    bool silent_ = true, nearFs_ = false;
    std::uint32_t clipRun_ = 0, clipRunPublished_ = 0;
    // FFT front end
    Fft fft_;
    std::array<std::vector<float>, 2> ring_;
    std::vector<float> win_, a_, b_, pA_, pB_, pw_, prevL_;
    std::size_t wpos_ = 0, hopCount_ = 0;
    float bandScale_ = 0.f;
    std::size_t bandLo_[3] = {0, 0, 0}, bandHi_[3] = {0, 0, 0}, fluxHi_ = 0;
    std::array<std::size_t, AnalyzerConstants::kFluxBands + 1> fluxEdge_{};
    std::array<int, AnalyzerConstants::kSpectrumBins> spLo_{}, spHi_{};
    std::array<float, AnalyzerConstants::kSpectrumBins> spFrac_{}, spec_{};
    float bands_[3] = {0, 0, 0};
    bool prevValid_ = false;
    // onsets
    std::array<float, AnalyzerConstants::kFluxHistory> fluxHist_{};
    std::size_t fluxIdx_ = 0, fluxFilled_ = 0;
    float fluxPrev1_ = 0.f, fluxPrev2_ = 0.f, curStrength_ = 0.f, latchedStrength_ = 0.f;
    bool onsetLatched_ = false;
    std::uint64_t lastOnsetSample_ = 0; bool haveOnset_ = false; float lastOnsetFlux_ = 0.f;
    std::size_t refractorySamples_ = 0;
    std::vector<OnsetEvent> events_;
    std::size_t evHead_ = 0, evCount_ = 0;
    // time bookkeeping
    std::uint64_t clock_ = 0, expectedNext_ = 0;
    bool haveIndex_ = false;
    Ns timeBaseNs_ = 0;
    // sub-analysers
    TempoEstimator tempo_;
    KeyEstimator key_;
    EnergyArc arc_;
    std::array<float, 512> monoStage_{};
    std::size_t monoN_ = 0;
    AnalyzerStats stats_;
};

}  // namespace ss::audio
