#include "ss/dynamics/truepeak.hpp"
#include <algorithm>
#include <cmath>

namespace ss::dynamics {

namespace {
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 80; ++k) {
        const double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}
}  // namespace

template <int kOS, int kTaps, int kBeta>
OversampledPeak<kOS, kTaps, kBeta>::OversampledPeak() {
    constexpr int N = kOversample * kTapsPerPhase;   // centre at tap N/2 (grid aligned)
    constexpr double beta = kBeta / 10.0;
    const double pi = 3.14159265358979323846;
    std::array<double, static_cast<std::size_t>(N)> h{};
    const double c = N / 2;
    for (int i = 0; i < N; ++i) {
        const double t = (i - c) / kOversample;      // input-sample units
        const double s = std::fabs(t) < 1e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
        const double r = (i - c) / c;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
        h[static_cast<std::size_t>(i)] = s * w;
    }
    // Unity DC gain per phase, so a constant input reads exactly as itself.
    for (int p = 0; p < kOversample; ++p) {
        double sum = 0;
        for (int k = 0; k < kTapsPerPhase; ++k) sum += h[static_cast<std::size_t>(p + k * kOversample)];
        for (int k = 0; k < kTapsPerPhase; ++k)
            coef_[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)] =
                static_cast<float>(h[static_cast<std::size_t>(p + k * kOversample)] / sum);
    }
    reset();
}

template <int kOS, int kTaps, int kBeta>
void OversampledPeak<kOS, kTaps, kBeta>::reset() { hist_.fill(0.0f); pos_ = 0; peak_ = 0.0f; }

template <int kOS, int kTaps, int kBeta>
float OversampledPeak<kOS, kTaps, kBeta>::push(float x) {
    pos_ = (pos_ + 1) % kTapsPerPhase;
    hist_[static_cast<std::size_t>(pos_)] = x;
    hist_[static_cast<std::size_t>(pos_ + kTapsPerPhase)] = x;
    float best = 0.0f;
    for (int p = 0; p < kOversample; ++p) {
        // y_p[n] = sum_j h[p + kOversample*j] * x[n - j]; x[n - j] lives at hist_[pos_ + kTapsPerPhase - j]
        float acc = 0.0f;
        const auto& c = coef_[static_cast<std::size_t>(p)];
        for (int j = 0; j < kTapsPerPhase; ++j)
            acc += c[static_cast<std::size_t>(j)] * hist_[static_cast<std::size_t>(pos_ + kTapsPerPhase - j)];
        best = std::max(best, std::fabs(acc));
    }
    peak_ = std::max(peak_, best);
    return best;
}

template class OversampledPeak<4, 12, 80>;
template class OversampledPeak<8, 32, 30>;

float truePeakOf(const float* x, std::size_t n) {
    TruePeakMeter m;
    for (std::size_t i = 0; i < n; ++i) m.push(x[i]);
    for (int i = 0; i < 2 * TruePeakMeter::kTapsPerPhase; ++i) m.push(0.0f);
    return m.peak();
}

}  // namespace ss::dynamics
