#include "ss/dynamics/sync.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ss::dynamics {

namespace {
double medianInPlace(std::vector<double>& v, std::size_t n) {
    if (n == 0) return 0.0;
    auto mid = v.begin() + static_cast<std::ptrdiff_t>(n / 2);
    std::nth_element(v.begin(), mid, v.begin() + static_cast<std::ptrdiff_t>(n));
    double m = *mid;
    if (n % 2 == 0) {
        const double lo = *std::max_element(v.begin(), mid);
        m = 0.5 * (m + lo);
    }
    return m;
}
}  // namespace

// ---- ClockMap ---------------------------------------------------------------------------------------
ClockMap::ClockMap(const ClockMapConfig& cfg) : cfg_(cfg) {
    cfg_.capacity = std::clamp<std::uint32_t>(cfg_.capacity, 8, 1024);
    if (cfg_.nominalRateHz == 0) cfg_.nominalRateHz = 48000;
    pos_.assign(cfg_.capacity, 0);
    host_.assign(cfg_.capacity, 0);
    scratch_.assign(static_cast<std::size_t>(cfg_.capacity) * cfg_.capacity / 2 + cfg_.capacity, 0.0);
    reset();
}

void ClockMap::reset() {
    n_ = 0; rejected_ = outliers_ = 0; lastPos_ = 0; lastStoredHost_ = 0;
    dirty_ = true; est_ = ClockEstimate{}; a_ = b_ = 0.0;
}

bool ClockMap::addPacket(std::uint64_t samplePos, Ns hostNs) {
    if (est_.valid) {   // judged against the most recent fit (refreshed whenever estimate() is called)
        const double predicted = a_ + b_ * static_cast<double>(samplePos - pos_[0]);
        if (std::fabs(static_cast<double>(hostNs) - predicted) > 50e6) ++outliers_;
    }
    if (n_ > 0) {
        if (samplePos <= lastPos_ || hostNs <= lastStoredHost_) { ++rejected_; return false; }   // not monotonic vs the last stored pair
        if (hostNs - lastStoredHost_ < cfg_.minPairIntervalNs) return false;                      // decimated
    }
    if (n_ == cfg_.capacity) {
        std::copy(pos_.begin() + 1, pos_.end(), pos_.begin());
        std::copy(host_.begin() + 1, host_.end(), host_.begin());
        --n_;
    }
    pos_[n_] = samplePos; host_[n_] = hostNs; ++n_;
    lastPos_ = samplePos;
    lastStoredHost_ = hostNs;
    dirty_ = true;
    return true;
}

void ClockMap::refit() const {
    dirty_ = false;
    est_ = ClockEstimate{};
    est_.pairs = static_cast<std::uint32_t>(n_);
    if (n_ == 0) return;
    est_.spanNs = host_[n_ - 1] - host_[0];
    const double nominal = 1e9 / cfg_.nominalRateHz;
    b_ = nominal;
    const bool enough = n_ >= cfg_.minPairs && est_.spanNs >= cfg_.minSpanNs;
    if (enough) {
        const std::size_t sep = std::max<std::size_t>(1, static_cast<std::size_t>(cfg_.minSeparationFraction * static_cast<double>(n_)));
        std::size_t m = 0;
        for (std::size_t i = 0; i < n_; ++i)
            for (std::size_t j = i + sep; j < n_; ++j) {
                const double dx = static_cast<double>(pos_[j] - pos_[i]);
                if (dx > 0) scratch_[m++] = static_cast<double>(host_[j] - host_[i]) / dx;
            }
        if (m > 0) b_ = medianInPlace(scratch_, m);
    }
    // intercept: median of residual offsets (robust to timestamp outliers)
    const double x0 = static_cast<double>(pos_[0]);
    for (std::size_t i = 0; i < n_; ++i)
        scratch_[i] = static_cast<double>(host_[i]) - b_ * (static_cast<double>(pos_[i]) - x0);
    a_ = medianInPlace(scratch_, n_);
    if (enough) {
        for (std::size_t i = 0; i < n_; ++i)
            scratch_[i] = std::fabs(static_cast<double>(host_[i]) - (a_ + b_ * (static_cast<double>(pos_[i]) - x0)));
        est_.residualMadNs = medianInPlace(scratch_, n_);
        est_.valid = true;
        est_.nsPerSample = b_;
        est_.measuredRateHz = 1e9 / b_;
        est_.ratePpm = (est_.measuredRateHz / cfg_.nominalRateHz - 1.0) * 1e6;
    }
}

const ClockEstimate& ClockMap::estimate() const {
    if (dirty_) refit();
    return est_;
}

Ns ClockMap::hostNsForSample(std::uint64_t samplePos) const {
    if (n_ == 0) return 0;
    if (dirty_) refit();
    const double dx = static_cast<double>(samplePos) - static_cast<double>(pos_[0]);
    return static_cast<Ns>(std::llround(a_ + b_ * dx));
}

std::uint64_t ClockMap::sampleForHostNs(Ns hostNs) const {
    if (n_ == 0) return 0;
    if (dirty_) refit();
    const double dx = (static_cast<double>(hostNs) - a_) / b_;
    const double pos = static_cast<double>(pos_[0]) + dx;
    return pos <= 0.0 ? 0 : static_cast<std::uint64_t>(std::llround(pos));
}

// ---- DriftController --------------------------------------------------------------------------------
const DriftDecision& DriftController::update(const ClockEstimate& est) {
    ++updates_;
    if (!est.valid) {   // keep whatever correction was in force; do not guess
        d_.state = d_.error ? DriftState::Error : (d_.appliedPpm != 0.0 ? DriftState::Correcting : DriftState::Unknown);
        d_.reason = "no valid estimate yet; holding previous correction";
        return d_;
    }
    const double m = est.ratePpm;
    d_.measuredPpm = m;
    const double mag = std::fabs(m);
    if (d_.error) {
        if (mag < cfg_.maxCorrectionPpm - cfg_.exitErrorMarginPpm) d_.error = false;
    } else if (mag > cfg_.maxCorrectionPpm) {
        d_.error = true;
        ++errorEntries_;
    }
    if (d_.error) {
        d_.state = DriftState::Error;
        d_.reason = "clock mismatch exceeds the resampling bound; resampling is not applied to hide it";
        // keep the already-applied correction (never jumps) and surface the error
        d_.inPerOut = 1.0 + d_.appliedPpm * 1e-6;
        return d_;
    }
    const double target = mag < cfg_.deadbandPpm ? 0.0 : m;
    const double step = std::clamp(target - d_.appliedPpm, -cfg_.maxSlewPpmPerUpdate, cfg_.maxSlewPpmPerUpdate);
    if (step != 0.0) { d_.appliedPpm += step; ++changes_; }
    d_.inPerOut = 1.0 + d_.appliedPpm * 1e-6;
    if (d_.appliedPpm == 0.0) { d_.state = DriftState::Locked; d_.reason = "within deadband; no resampling"; }
    else { d_.state = DriftState::Correcting; d_.reason = "gentle resampling within bound"; }
    return d_;
}

// ---- Delay compensation -----------------------------------------------------------------------------
std::string DelayCompensation::toJson() const {
    char buf[256];
    std::snprintf(buf, sizeof buf,
        "{\"rateHz\":%u,\"lookaheadSamples\":%u,\"filterSamples\":%u,\"codecPrimingSamples\":%u,\"totalSamples\":%llu,\"totalNs\":%lld}",
        rateHz, lookaheadSamples, filterSamples, codecPrimingSamples,
        static_cast<unsigned long long>(totalSamples()), static_cast<long long>(totalNs()));
    return buf;
}

DelayCompensation makeDelayCompensation(const PrepareInfo& dyn, std::uint32_t filterSamples, std::uint32_t codecPrimingSamples) {
    DelayCompensation d;
    d.rateHz = dyn.sampleRateHz ? dyn.sampleRateHz : 48000;
    d.lookaheadSamples = dyn.processingDelaySamples;
    d.filterSamples = filterSamples;
    d.codecPrimingSamples = codecPrimingSamples;
    return d;
}

// ---- LinearResampler --------------------------------------------------------------------------------
LinearResampler::LinearResampler(std::uint32_t channels) : ch_(std::clamp<std::uint32_t>(channels, 1, 8)), prev_(ch_, 0.0f) {}

void LinearResampler::reset() { pos_ = 0; std::fill(prev_.begin(), prev_.end(), 0.0f); }

void LinearResampler::setRatio(double inPerOut) {
    ratio_ = std::isfinite(inPerOut) ? std::clamp(inPerOut, 0.5, 2.0) : 1.0;
    step_ = static_cast<std::uint64_t>(std::llround(ratio_ * 4294967296.0));
}

LinearResampler::Io LinearResampler::process(const float* in, std::size_t nIn, float* out, std::size_t outCap) {
    // virtual stream: v[0] = prev_, v[j] = in[j-1]; pos_ is the Q32 read position measured from v[0].
    Io io;
    std::uint64_t pos = pos_;
    std::size_t produced = 0;
    while (produced < outCap) {
        const std::size_t i0 = static_cast<std::size_t>(pos >> 32);
        if (i0 + 1 > nIn) break;                           // need v[i0+1] = in[i0]
        const double f = static_cast<double>(pos & 0xFFFFFFFFull) / 4294967296.0;
        for (std::uint32_t c = 0; c < ch_; ++c) {
            const float a = i0 == 0 ? prev_[c] : in[(i0 - 1) * ch_ + c];
            const float b = in[i0 * ch_ + c];
            out[produced * ch_ + c] = static_cast<float>(a + (static_cast<double>(b) - a) * f);
        }
        ++produced;
        pos += step_;
    }
    const std::size_t shift = std::min(static_cast<std::size_t>(pos >> 32), nIn);
    if (shift > 0) {
        for (std::uint32_t c = 0; c < ch_; ++c) prev_[c] = in[(shift - 1) * ch_ + c];
        pos -= static_cast<std::uint64_t>(shift) << 32;
    }
    pos_ = pos;
    io.consumed = shift;
    io.produced = produced;
    return io;
}

}  // namespace ss::dynamics
