// StageSignal record: output-rate estimation and disk thresholds (spec section 12, REQ-R04).
//   warn at 2 GiB free OR an estimated 10 minutes remaining;
//   graceful-stop reserve = max(256 MiB, estimated 30 s of output bytes);
//   estimates use the MEASURED output rate and always carry an uncertainty.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include "ss/base/time.hpp"
#include "ss/record/profile.hpp"

namespace ss::record {

struct RateEstimate {
    double bytesPerSec = 0;
    double relUncertainty = 1.0;     // ~95% half-width as a fraction of bytesPerSec (0.2 = +/-20%)
    bool measured = false;           // false until >= 5 s of samples exist: then this is the nominal profile rate
    double upper() const { return bytesPerSec * (1.0 + relUncertainty); }
};

class RateEstimator {
public:
    RateEstimator(double nominalBytesPerSec, double nominalRelUncertainty)
        : nominal_(nominalBytesPerSec), nominalUnc_(nominalRelUncertainty) {}
    // totalBytes = cumulative bytes written by this session (all segments). Samples closer than 500 ms are folded.
    void addSample(Ns now, std::uint64_t totalBytes);
    RateEstimate estimate() const;
private:
    struct Sample { Ns t; std::uint64_t bytes; };
    double nominal_, nominalUnc_;
    std::deque<Sample> samples_;      // sliding window, <= 120 entries
};

enum class DiskLevel : std::uint8_t { Ok, Warning, StopNow };

struct DiskStatus {
    DiskLevel level = DiskLevel::Ok;
    std::uint64_t freeBytes = 0;
    std::uint64_t reserveBytes = 0;
    RateEstimate rate;
    Ns remainingPointNs = 0;          // (free - reserve) / rate
    Ns remainingLowNs = 0;            // pessimistic: uses rate * (1 + uncertainty)
    bool warnByFree = false, warnByTime = false;
    std::string message;              // operator-facing, no paths
};

class DiskMonitor {
public:
    DiskMonitor(double nominalBytesPerSec, double nominalRelUncertainty) : rate_(nominalBytesPerSec, nominalRelUncertainty) {}
    void onBytesWritten(Ns now, std::uint64_t totalBytes) { rate_.addSample(now, totalBytes); }
    DiskStatus update(std::uint64_t freeBytes) const;
    static std::uint64_t reserveFor(const RateEstimate& r);
private:
    RateEstimator rate_;
};

// Nominal output rate of a profile (bytes/s) and how much to trust it before measurements exist.
struct NominalRate { double bytesPerSec = 0; double relUncertainty = 0; };
NominalRate nominalRate(const RecordProfile& p);                       // before the encoder is known: policy-based, pessimistic
NominalRate nominalRateFor(const RecordProfile& p, VideoCodec codec);  // after selectVideoEncoder

}  // namespace ss::record
