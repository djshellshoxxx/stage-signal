// 4x polyphase oversampled true-peak METER (BS.1770 style estimate). This is a meter only:
// the dynamics limiter is a sample-peak limiter and makes no true-peak guarantee (docs/specs/dynamics.md).
// The interpolation FIR is a Kaiser-windowed sinc designed here (48 taps, 12 per phase); it is not the
// normative BS.1770 Annex 2 coefficient set, so readings are an estimate whose error is measured in tests.
// Output is delayed by kDelaySamples input samples (phase 0 is the input delayed by 6 samples).
#pragma once
#include <array>
#include <cstddef>

namespace ss::dynamics {

class TruePeakMeter {
public:
    static constexpr int kOversample = 4;
    static constexpr int kTapsPerPhase = 12;
    static constexpr int kDelaySamples = 6;

    TruePeakMeter();                 // builds coefficients; no allocation at all
    void reset();
    // Feed one sample; returns the largest |value| among the 4 interpolated phases of this step.
    float push(float x);
    float peak() const { return peak_; }   // running maximum since reset()/resetPeak()
    void resetPeak() { peak_ = 0.0f; }

private:
    std::array<std::array<float, kTapsPerPhase>, kOversample> coef_{};
    std::array<float, kTapsPerPhase * 2> hist_{};   // doubled ring so reads are contiguous
    int pos_ = 0;
    float peak_ = 0.0f;
};

// Offline helper: true-peak estimate of a whole mono buffer (flushes the filter tail).
float truePeakOf(const float* x, std::size_t n);

}  // namespace ss::dynamics
