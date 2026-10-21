#include "ss/diag/sampler.hpp"

#include <algorithm>

namespace ss::diag {

bool TempoKeySampler::offer(Ns monoNs, const FeatureSnapshot& snap, std::uint32_t sceneId, FeatureSample& out) {
    if (haveLast_ && monoNs < last_ + kMinSampleIntervalNs) return false;
    haveLast_ = true;
    last_ = monoNs;
    out = FeatureSample{};
    out.monoNs = monoNs;
    out.bpm = snap.tempo.valid() ? snap.tempo.bpm : 0.0;
    if (snap.key.valid()) { out.keyTonic = snap.key.tonic; out.keyMinor = snap.key.minor; }
    out.energyArc = std::min(1.f, std::max(0.f, snap.energyArc));
    out.arcPhase = snap.arcPhase;
    out.sceneId = sceneId;
    return true;
}

FeatureHistory::FeatureHistory(std::size_t capacity) : buf_(std::max<std::size_t>(capacity, 2)) {}

bool FeatureHistory::offer(Ns monoNs, const FeatureSnapshot& snap, std::uint32_t sceneId) {
    FeatureSample s;
    if (!sampler_.offer(monoNs, snap, sceneId, s)) return false;
    if (count_ == buf_.size()) ++overwritten_; else ++count_;
    buf_[head_] = s;
    head_ = (head_ + 1) % buf_.size();
    return true;
}

std::vector<FeatureSample> FeatureHistory::samples() const {
    std::vector<FeatureSample> out;
    out.reserve(count_);
    const std::size_t start = count_ == buf_.size() ? head_ : 0;
    for (std::size_t i = 0; i < count_; ++i) out.push_back(buf_[(start + i) % buf_.size()]);
    return out;
}

void FeatureHistory::clear() { head_ = 0; count_ = 0; sampler_.reset(); }

}  // namespace ss::diag
