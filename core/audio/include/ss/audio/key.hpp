// StageSignal audio: key estimation (REQ-A07) and manual key override (REQ-A08).
// Spec: docs/specs/audio.md "Key". Major/minor only; no chord recognition; no claim from drums/noise.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ss/audio/fft.hpp"
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"

namespace ss::audio {

struct KeyLimits {
    static constexpr double kDecimatedRateHz = 4000.0;   // analysis rate after integer decimation
    static constexpr std::size_t kFrame = 4096;          // FFT size at the decimated rate (~1.02 s)
    static constexpr std::size_t kHop = 2048;            // ~0.51 s
    static constexpr std::size_t kRingFrames = 24;       // chroma context ~12.3 s (spec: 8-20 s)
    static constexpr std::size_t kMinActiveFrames = 16;  // ~8.2 s of non-silent audio before any claim
    static constexpr std::size_t kEvalEveryFrames = 4;   // re-evaluate every ~2 s
    static constexpr double kMinFreqHz = 60.0, kMaxFreqHz = 1600.0;
    static constexpr float kActiveRms = 0.001f;          // decimated frame RMS (-60 dBFS) below = inactive
    // Reporting threshold: a key is only reported while its smoothed confidence is >= this value.
    static constexpr float kMinConfidence = 0.50f;
};

// Diagnostics of the last evaluation (for tests and the calibration tooling; not part of the contract).
struct KeyDiagnostics {
    double bestR = 0.0, secondR = 0.0;       // Pearson correlation of the best / second best template
    int bestTonic = -1; bool bestMinor = false;
    double candidateConfidence = 0.0;
    std::size_t activeFrames = 0;
    int tuningOffset = 0;                     // -1, 0, +1 third-semitones
};

// Runs on a worker/analysis thread. process() is allocation free after prepare() but performs one
// 4096-point FFT per ~0.5 s of audio (sub-millisecond spike); do not call from a hard real-time callback.
class KeyEstimator {
public:
    bool prepare(std::uint32_t rateHz);
    void reset();
    // Mono audio at the prepared rate; NaN/Inf must already be sanitised by the caller (they are treated as 0 here).
    void process(const float* mono, std::size_t n);
    KeyState state() const;
    const KeyDiagnostics& diagnostics() const { return diag_; }
    // Temperley (2005, as used by the Melisma/MIREX key finders) major/minor key profiles, tonic first.
    // Chosen over Krumhansl-Kessler because it separates a key from its relative minor/major by a larger margin
    // on triad-based material (measured on the synthetic progressions in the unit tests).
    static const std::array<double, 12>& majorProfile();
    static const std::array<double, 12>& minorProfile();

private:
    void frame();
    void evaluate();
    Ns nowNs() const { return samplesToNs(inSamples_, rate_); }

    std::uint32_t rate_ = 0, decim_ = 1;
    double fsd_ = 0.0;
    // 2 cascaded RBJ low-pass biquads (Butterworth 4th order)
    struct Bq {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float run(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            if (std::fabs(z1) < 1e-20f) { z1 = 0.f; }      // flush denormals
            if (std::fabs(z2) < 1e-20f) { z2 = 0.f; }
            return y;
        }
    };
    std::array<Bq, 2> lp_;
    std::uint32_t decimPhase_ = 0;
    std::vector<float> ring_, win_, fbuf_, pow_;
    std::size_t wpos_ = 0, decCount_ = 0, hopCount_ = 0;
    Fft fft_;
    std::vector<std::int16_t> binMap_;       // FFT bin -> chroma36 bin (or -1)
    std::size_t kLo_ = 0, kHi_ = 0;
    std::array<std::array<float, 36>, KeyLimits::kRingFrames> chroma_{};
    std::array<bool, KeyLimits::kRingFrames> active_{};
    std::size_t slot_ = 0, framesSinceEval_ = 0, framesTotal_ = 0;
    std::uint64_t inSamples_ = 0;
    // reported key state
    int tonic_ = -1; bool minor_ = false; double conf_ = 0.0; Ns confirmNs_ = 0;
    int candTonic_ = -1; bool candMinor_ = false; int candRun_ = 0;
    KeyDiagnostics diag_;
};

// Manual key override (REQ-A08). Deterministic; persistence of the override itself is done by the show module.
class KeyController {
public:
    Status setManual(int tonic, bool minor, Ns nowNs);   // tonic 0..11 else InvalidArgument
    void release();
    KeyState resolve(const KeyState& estimate, Ns nowNs) const;
    ValueSource overrideSource() const { return manual_ ? ValueSource::Manual : ValueSource::Unknown; }
private:
    bool manual_ = false, minor_ = false;
    int tonic_ = -1;
    Ns setNs_ = 0;
};

}  // namespace ss::audio
