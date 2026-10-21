// StageSignal diag: tempo/key snapshot sampling at <= 1 Hz (spec section 17) and a bounded feature history that feeds
// the Show Report. Callers offer every FeatureSnapshot they produce; the sampler keeps at most one per second.
#pragma once
#include <cstdint>
#include <vector>
#include "ss/base/types.hpp"

namespace ss::diag {

constexpr Ns kMinSampleIntervalNs = kNsPerSec;           // <= 1 Hz
constexpr std::size_t kDefaultHistoryCapacity = 6 * 3600;   // six hours at 1 Hz

struct FeatureSample {
    Ns monoNs = 0;
    double bpm = 0.0;               // 0 = no valid tempo (never a numeric zero BPM)
    int keyTonic = -1;              // 0=C .. 11=B, -1 unknown
    bool keyMinor = false;
    float energyArc = 0.f;          // 0..1
    std::uint8_t arcPhase = 0;      // 0 steady, 1 build, 2 drop
    std::uint32_t sceneId = 0;
};

class TempoKeySampler {
public:
    // Returns true and fills `out` when at least kMinSampleIntervalNs passed since the last emitted sample.
    // A monotonic time that goes backwards is ignored (never emitted), so the output is strictly increasing.
    bool offer(Ns monoNs, const FeatureSnapshot& snap, std::uint32_t sceneId, FeatureSample& out);
    void reset() { haveLast_ = false; }
private:
    bool haveLast_ = false;
    Ns last_ = 0;
};

// Ring of samples; oldest are overwritten (and counted) once capacity is reached.
class FeatureHistory {
public:
    explicit FeatureHistory(std::size_t capacity = kDefaultHistoryCapacity);
    bool offer(Ns monoNs, const FeatureSnapshot& snap, std::uint32_t sceneId);
    std::vector<FeatureSample> samples() const;          // oldest first
    std::size_t size() const { return count_; }
    std::uint64_t overwritten() const { return overwritten_; }
    void clear();
private:
    TempoKeySampler sampler_;
    std::vector<FeatureSample> buf_;
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t overwritten_ = 0;
};

}  // namespace ss::diag
