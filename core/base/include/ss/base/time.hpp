// StageSignal base: integer-nanosecond time model and injectable clocks (spec section 10).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace ss {

using Ns = std::int64_t;  // monotonic nanoseconds unless a name says wall-clock

constexpr Ns kNsPerSec = 1'000'000'000LL;
constexpr Ns kNsPerMs = 1'000'000LL;

constexpr Ns msToNs(std::int64_t ms) { return ms * kNsPerMs; }
constexpr Ns secToNs(std::int64_t s) { return s * kNsPerSec; }
constexpr double nsToSec(Ns ns) { return static_cast<double>(ns) / 1e9; }
constexpr double nsToMs(Ns ns) { return static_cast<double>(ns) / 1e6; }

// Sample index -> nanoseconds without accumulating rounded float durations.
constexpr Ns samplesToNs(std::uint64_t sampleIndex, std::uint32_t rateHz) {
    const std::uint64_t whole = sampleIndex / rateHz;
    const std::uint64_t rem = sampleIndex % rateHz;
    return static_cast<Ns>(whole * 1'000'000'000ULL + (rem * 1'000'000'000ULL) / rateHz);
}

constexpr std::uint64_t nsToSamples(Ns ns, std::uint32_t rateHz) {
    const std::uint64_t u = static_cast<std::uint64_t>(ns < 0 ? 0 : ns);
    const std::uint64_t whole = u / 1'000'000'000ULL;
    const std::uint64_t rem = u % 1'000'000'000ULL;
    return whole * rateHz + (rem * rateHz) / 1'000'000'000ULL;
}

// Beat duration in ns for a tempo. bpm must be > 0.
constexpr Ns beatNs(double bpm) { return static_cast<Ns>(60.0 * 1e9 / bpm); }

class IClock {
public:
    virtual ~IClock() = default;
    virtual Ns nowMonotonic() const = 0;   // QPC-like, never goes backwards
    virtual Ns wallUnixNs() const = 0;     // wall clock, for journal/filenames only
};

class SystemClock final : public IClock {
public:
    Ns nowMonotonic() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    Ns wallUnixNs() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

// Deterministic clock for simulation tests.
class FakeClock final : public IClock {
public:
    explicit FakeClock(Ns startMono = 0, Ns startWall = 1'700'000'000LL * kNsPerSec)
        : mono_(startMono), wall_(startWall) {}
    Ns nowMonotonic() const override { return mono_.load(std::memory_order_relaxed); }
    Ns wallUnixNs() const override { return wall_.load(std::memory_order_relaxed); }
    void advance(Ns d) { mono_ += d; wall_ += d; }
    void set(Ns mono) { mono_ = mono; }
private:
    std::atomic<Ns> mono_;
    std::atomic<Ns> wall_;
};

}  // namespace ss
