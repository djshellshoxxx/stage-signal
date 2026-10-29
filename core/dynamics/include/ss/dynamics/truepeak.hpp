// Polyphase oversampled true-peak detection: a 4x METER (BS.1770 style estimate) and the 8x detector the optional
// true-peak limiter uses (docs/specs/dynamics.md).
// The interpolation FIR is a Kaiser-windowed sinc designed here (48 taps, 12 per phase); it is not the
// normative BS.1770 Annex 2 coefficient set, so readings are an estimate whose error is measured in tests.
// Output is delayed by kDelaySamples input samples (phase 0 is the input delayed by 6 samples).
#pragma once
#include <array>
#include <cstddef>

namespace ss::dynamics {

// Polyphase oversampling peak detector, `kOS`x with `kTaps` taps per phase. One implementation serves the 4x meter and the 8x
// detector inside the true-peak limiter (no second copy of the filter code).
// `kBetaTenths`: Kaiser window beta x 10. A larger beta rejects images harder but narrows the flat band, so the 4x meter keeps
// the BS.1770-like 8.0 and the limiter detector uses a gentler 3.0 to stay accurate up to 0.49 fs (measured offline, see processor.cpp).
template <int kOS, int kTaps, int kBetaTenths>
class OversampledPeak {
public:
    static constexpr int kOversample = kOS;
    static constexpr int kTapsPerPhase = kTaps;
    static constexpr int kDelaySamples = kTaps / 2;

    OversampledPeak();               // builds coefficients; no allocation at all
    void reset();
    // Feed one sample; returns the largest |value| among the kOS interpolated phases of this step. Phase 0 is the input
    // delayed by kDelaySamples; the phases lie between that sample and the next one.
    float push(float x);
    float peak() const { return peak_; }   // running maximum since reset()/resetPeak()
    void resetPeak() { peak_ = 0.0f; }

private:
    std::array<std::array<float, kTaps>, kOS> coef_{};
    std::array<float, kTaps * 2> hist_{};   // doubled ring so reads are contiguous
    int pos_ = 0;
    float peak_ = 0.0f;
};

// 4x meter (BS.1770 style estimate, 48 taps). Meter only.
using TruePeakMeter = OversampledPeak<4, 12, 80>;
// 8x detector (256 taps) used by the true-peak limiter. Vs a 129-tap 16x reference: tones to 0.49 fs read within 0.1 dB, loud
// full-band white noise reads about 0.13 dB low.
using TruePeakDetector8 = OversampledPeak<8, 32, 30>;

// Offline helper: true-peak estimate of a whole mono buffer (flushes the filter tail).
float truePeakOf(const float* x, std::size_t n);

}  // namespace ss::dynamics
