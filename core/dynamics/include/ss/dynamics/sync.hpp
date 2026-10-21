// Sync model (spec section 12 "Clock model", REQ-R03): pure, deterministic logic. No clocks are read here;
// callers feed (sample position, host QPC ns) packet pairs and use the results to stamp audio/video.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/dynamics/processor.hpp"

namespace ss::dynamics {

// ---- ClockMap ------------------------------------------------------------------------------------
// Maps capture sample position -> host monotonic ns with a robust linear model host = a + b * (pos - pos0).
// Slope b comes from a Theil-Sen estimator over widely separated pairs (median of pairwise slopes), so packet
// timestamp jitter and isolated outliers do not bias the rate. Device rate error in ppm is
// (1e9 / b / nominalRate - 1) * 1e6: positive means the device clock runs fast relative to the host clock.
struct ClockMapConfig {
    std::uint32_t nominalRateHz = 48000;
    std::uint32_t capacity = 256;                 // stored pairs (sliding window), <= 1024
    Ns minPairIntervalNs = 500 * kNsPerMs;        // packets closer than this to the last stored pair are decimated
    Ns minSpanNs = 20 * kNsPerSec;                // estimate is not valid before this span ...
    std::uint32_t minPairs = 16;                  // ... and this many stored pairs
    double minSeparationFraction = 0.25;          // only pairs at least this fraction of the window apart vote
};

struct ClockEstimate {
    bool valid = false;
    double ratePpm = 0.0;           // device rate error vs nominal (valid only when `valid`)
    double measuredRateHz = 0.0;
    double nsPerSample = 0.0;
    double residualMadNs = 0.0;     // median absolute deviation of pairs from the fitted line (jitter)
    std::uint32_t pairs = 0;
    Ns spanNs = 0;
};

class ClockMap {
public:
    explicit ClockMap(const ClockMapConfig& cfg = {});
    void reset();
    // Returns true when the pair was stored. Pairs not strictly increasing vs the last stored pair are rejected
    // (counted); pairs closer than minPairIntervalNs to the last stored pair are decimated (not counted).
    bool addPacket(std::uint64_t samplePos, Ns hostNs);
    const ClockEstimate& estimate() const;
    // Prediction uses the fitted line when valid, otherwise nominal rate anchored at the latest pair. Returns 0 before any pair.
    Ns hostNsForSample(std::uint64_t samplePos) const;
    std::uint64_t sampleForHostNs(Ns hostNs) const;
    std::uint64_t rejectedNonMonotonic() const { return rejected_; }
    std::uint64_t outliers() const { return outliers_; }     // pairs >50 ms from the latest fit (fit refreshed by estimate())
    std::size_t size() const { return n_; }

private:
    void refit() const;
    ClockMapConfig cfg_;
    std::vector<std::uint64_t> pos_;
    std::vector<Ns> host_;
    std::size_t n_ = 0;
    std::uint64_t lastPos_ = 0, rejected_ = 0, outliers_ = 0;
    Ns lastStoredHost_ = 0;
    mutable bool dirty_ = true;
    mutable ClockEstimate est_;
    mutable double a_ = 0.0, b_ = 0.0;      // host(ns) = a + b * (pos - pos0)
    mutable std::vector<double> scratch_;
};

// ---- DriftController -----------------------------------------------------------------------------
enum class DriftState : std::uint8_t { Unknown, Locked, Correcting, Error };

struct DriftConfig {
    double maxCorrectionPpm = 200.0;      // spec: fixed resampling bounds +-200 ppm
    double deadbandPpm = 2.0;             // below this, no correction
    double exitErrorMarginPpm = 20.0;     // error clears when |measured| < max - margin
    double maxSlewPpmPerUpdate = 10.0;    // gentle: the applied correction moves at most this per update
};

struct DriftDecision {
    DriftState state = DriftState::Unknown;
    double measuredPpm = 0.0;
    double appliedPpm = 0.0;              // correction currently applied (same sign convention as measured)
    double inPerOut = 1.0;                // LinearResampler ratio: input frames consumed per output frame
    bool error = false;                   // |measured| beyond the bound: do not resample, surface an error
    const char* reason = "no estimate yet";
};

class DriftController {
public:
    explicit DriftController(const DriftConfig& cfg = {}) : cfg_(cfg) {}
    const DriftDecision& update(const ClockEstimate& est);
    const DriftDecision& decision() const { return d_; }
    std::uint64_t updates() const { return updates_; }
    std::uint64_t correctionChanges() const { return changes_; }   // recorded for metadata
    std::uint64_t errorEntries() const { return errorEntries_; }
private:
    DriftConfig cfg_;
    DriftDecision d_;
    std::uint64_t updates_ = 0, changes_ = 0, errorEntries_ = 0;
};

// ---- Delay compensation --------------------------------------------------------------------------
// Audio emerges from the chain `totalSamples()` after it was captured. Stamping audio by output position makes it
// late by that amount relative to video, so either audio PTS is moved earlier (audioPtsNs) or video PTS is delayed
// (videoPtsOffsetNs). Do exactly one of them.
struct DelayCompensation {
    std::uint32_t rateHz = 48000;
    std::uint32_t lookaheadSamples = 0;       // DynamicsProcessor::processingDelaySamples
    std::uint32_t filterSamples = 0;          // other filter group delay (resampler, etc.)
    std::uint32_t codecPrimingSamples = 0;    // encoder priming / pre-roll
    std::uint64_t totalSamples() const { return std::uint64_t{lookaheadSamples} + filterSamples + codecPrimingSamples; }
    Ns totalNs() const { return samplesToNs(totalSamples(), rateHz); }
    // PTS for the audio sample at output stream position `outputSampleIndex` (stream-relative, ns).
    Ns audioPtsNs(std::uint64_t outputSampleIndex) const { return samplesToNs(outputSampleIndex, rateHz) - totalNs(); }
    Ns videoPtsOffsetNs() const { return totalNs(); }
    std::string toJson() const;
};

DelayCompensation makeDelayCompensation(const PrepareInfo& dyn, std::uint32_t filterSamples, std::uint32_t codecPrimingSamples);

// ---- Linear resampler ----------------------------------------------------------------------------
// Streaming linear interpolation for tiny ratio corrections (|ratio-1| <= 200 ppm in practice). The first output has one
// frame of zero history. Allocation-free after construction.
class LinearResampler {
public:
    explicit LinearResampler(std::uint32_t channels = 2);
    void reset();
    void setRatio(double inPerOut);          // clamped to [0.5, 2]
    double ratio() const { return ratio_; }
    struct Io { std::size_t consumed = 0, produced = 0; };
    // Produces up to outCap frames; consumes only as many input frames as needed (consumed <= nIn). Call again with the rest.
    Io process(const float* in, std::size_t nIn, float* out, std::size_t outCap);
private:
    std::uint32_t ch_;
    double ratio_ = 1.0;
    std::uint64_t step_ = 1ull << 32;   // Q32 fixed point: exact, chunk-independent phase
    std::uint64_t pos_ = 0;             // Q32 position measured from the stored previous frame
    std::vector<float> prev_;
};

}  // namespace ss::dynamics
