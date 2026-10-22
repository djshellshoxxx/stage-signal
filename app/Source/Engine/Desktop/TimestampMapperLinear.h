// Sample index -> monotonic ns by a sliding linear fit of callback entry times (app-adapters 3.2, APP-AD-03).
// map() runs on the audio thread: fixed arrays, no allocation, no lock. Valid only while Converged.
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "Engine/ITimestampMapper.h"

namespace ss::app {

class LinearFitTimestampMapper final : public ITimestampMapper {
public:
    static constexpr int kN = 256;                       // window; >= 1 s for buffers up to 1024 frames at 48 kHz
    static constexpr double kMinSpanSec = 1.0;
    static constexpr double kConvergedRmsSec = 0.002, kDegradedRmsSec = 0.005;

    // Reported input latency + buffer duration: capture time = fitted callback entry - this. Any thread.
    void setLatencyNs(ss::Ns n) { latencyNs_.store(n, std::memory_order_relaxed); }

    MappedTime map(std::uint64_t firstSample, std::uint32_t rateHz, ss::Ns entryNs) override {
        if (resetFlag_.exchange(false, std::memory_order_acq_rel)) restart();
        if (rateHz == 0) return {entryNs, false};
        if (n_ == 0) { idx0_ = firstSample; t0_ = entryNs; }
        const double x = (static_cast<double>(firstSample) - static_cast<double>(idx0_)) / static_cast<double>(rateHz);
        const double y = static_cast<double>(entryNs - t0_) * 1e-9;
        // Outlier rejection (3 sigma and at least 1 ms off the current prediction) once a fit exists.
        if (haveFit_ && n_ >= 32) {
            const double resid = std::fabs(y - (a_ + b_ * x));
            if (resid > 3.0 * sigma_ && resid > 0.001 && consecutiveOutliers_ < 16) {
                ++consecutiveOutliers_;
                outliers_.fetch_add(1, std::memory_order_relaxed);
                return out(x, entryNs);
            }
        }
        consecutiveOutliers_ = 0;
        xs_[static_cast<std::size_t>(head_)] = x; ys_[static_cast<std::size_t>(head_)] = y;
        head_ = (head_ + 1) % kN;
        if (n_ < kN) ++n_;
        refit(x);
        return out(x, entryNs);
    }

    void reset() override { resetFlag_.store(true, std::memory_order_release); state_.store(MapperState::Unconverged); }
    MapperState state() const override { return state_.load(std::memory_order_relaxed); }
    std::uint64_t outliersRejected() const { return outliers_.load(std::memory_order_relaxed); }

private:
    void restart() { n_ = 0; head_ = 0; haveFit_ = false; consecutiveOutliers_ = 0; state_.store(MapperState::Unconverged); }

    void refit(double xNow) {
        if (n_ < 8) return;
        // Centre on the newest x for numerical stability.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n_; ++i) { const double dx = xs_[static_cast<std::size_t>(i)] - xNow; const double yy = ys_[static_cast<std::size_t>(i)];
            sx += dx; sy += yy; sxx += dx * dx; sxy += dx * yy; }
        const double n = static_cast<double>(n_);
        const double den = n * sxx - sx * sx;
        if (den <= 1e-12) return;
        const double b = (n * sxy - sx * sy) / den;
        const double a0 = (sy - b * sx) / n;             // y at x == xNow
        double ss = 0, xmin = xNow;
        for (int i = 0; i < n_; ++i) { const double dx = xs_[static_cast<std::size_t>(i)] - xNow;
            const double r = ys_[static_cast<std::size_t>(i)] - (a0 + b * dx); ss += r * r; if (xs_[static_cast<std::size_t>(i)] < xmin) xmin = xs_[static_cast<std::size_t>(i)]; }
        b_ = b; a_ = a0 - b * xNow;                      // y = a + b x in absolute x
        sigma_ = std::sqrt(ss / n);
        if (sigma_ < 1e-5) sigma_ = 1e-5;
        haveFit_ = true;
        const double span = xNow - xmin;
        if (span < kMinSpanSec) { state_.store(MapperState::Unconverged, std::memory_order_relaxed); return; }
        const MapperState cur = state_.load(std::memory_order_relaxed);
        if (sigma_ <= kConvergedRmsSec) state_.store(MapperState::Converged, std::memory_order_relaxed);
        else if (sigma_ >= kDegradedRmsSec) state_.store(MapperState::Degraded, std::memory_order_relaxed);
        else if (cur == MapperState::Unconverged) state_.store(MapperState::Degraded, std::memory_order_relaxed);   // hysteresis band
    }

    MappedTime out(double x, ss::Ns entryNs) const {
        if (!haveFit_ || state_.load(std::memory_order_relaxed) != MapperState::Converged) return {entryNs, false};
        const double fitted = a_ + b_ * x;
        return {t0_ + static_cast<ss::Ns>(std::llround(fitted * 1e9)) - latencyNs_.load(std::memory_order_relaxed), true};
    }

    std::array<double, kN> xs_{}, ys_{};
    int n_ = 0, head_ = 0, consecutiveOutliers_ = 0;
    std::uint64_t idx0_ = 0;
    ss::Ns t0_ = 0;
    double a_ = 0, b_ = 1, sigma_ = 1;
    bool haveFit_ = false;
    std::atomic<bool> resetFlag_{false};
    std::atomic<MapperState> state_{MapperState::Unconverged};
    std::atomic<ss::Ns> latencyNs_{0};
    std::atomic<std::uint64_t> outliers_{0};
};

}  // namespace ss::app
