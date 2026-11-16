// Contract: ANALYSIS thread body (app-shell 4.1). Owner: shell-integration.
// Pulls blocks from AudioIngress, runs audio::Analyzer on the RAW capture, then dynamics::DynamicsProcessor, and
// hands PROCESSED blocks to registered consumers (record/broadcast taps). Publishes the latest FeatureSnapshot
// and AudioVisualInput through mailboxes. step() is thread-free so tests can drive it; AppController runs it in a loop.
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "App/AudioIngress.h"
#include "App/Mailboxes.h"
#include "ss/audio/analyzer.hpp"
#include "ss/dynamics/processor.hpp"
#include "ss/render/scene.hpp"

namespace ss::app {

// Implemented by recorders/encoders. Called on the ANALYSIS thread: copy into your own ring and return at once.
class IProcessedAudioConsumer {
public:
    virtual ~IProcessedAudioConsumer() = default;
    virtual void onProcessedBlock(const AudioBlockView& processed, const AudioBlockView& raw) = 0;
};

class AnalysisWorker {
public:
    AnalysisWorker(AudioIngress& in, const ss::IClock& clock) : in_(in), clock_(clock) {}

    void addConsumer(IProcessedAudioConsumer* c) { consumers_.push_back(c); }   // before the thread starts

    // Drains everything queued. Returns the number of blocks processed. ANALYSIS thread only.
    std::size_t step();

    // Any thread.
    bool features(ss::FeatureSnapshot& out) const { return features_.read(out); }
    bool visual(ss::render::AudioVisualInput& out) const { return visual_.read(out); }
    // Any thread, before audio starts (REQ-R02). Opt-in: adds the detector latency and about 4x the limiter's CPU; see
    // docs/specs/dynamics.md. Takes effect when the engine is next prepared (device or format change, or restart).
    void setTruePeakLimiter(bool on) { truePeakLimiter_.store(on, std::memory_order_relaxed); }
    bool signalAlive(ss::Ns now) const {
        const ss::Ns last = lastBlockNs_.load(std::memory_order_relaxed);
        return last != 0 && now - last < 500 * ss::kNsPerMs;
    }
    ss::dynamics::DynamicsTelemetry dynamicsTelemetry() const;       // copy, locked internally by the processor
    ss::dynamics::DynamicsProcessor& dynamics() { return dyn_; }     // setParams() is callable from any thread
    const ss::LatestMailbox<ss::FeatureSnapshot>& featureMailbox() const { return features_; }
    const ss::LatestMailbox<ss::render::AudioVisualInput>& visualMailbox() const { return visual_; }
    // Onset drain: every onset the analyser emitted is counted, so a consumer slower than the analysis loop never
    // misses one (the mailbox's onsetFired is edge-latched and can be overwritten). Each consumer keeps its own cursor.
    std::uint64_t onsetsSince(std::uint64_t& cursor) const {
        const std::uint64_t now = onsets_.load(std::memory_order_relaxed);
        const std::uint64_t n = now - cursor;
        cursor = now;
        return n;
    }
    std::uint64_t blocksProcessed() const { return blocks_.load(std::memory_order_relaxed); }

private:
    bool ensurePrepared(const AudioBlockView& v);
    AudioIngress& in_;
    const ss::IClock& clock_;
    ss::audio::Analyzer analyzer_;
    ss::dynamics::DynamicsProcessor dyn_;
    std::vector<IProcessedAudioConsumer*> consumers_;
    std::vector<float> out_;                       // preallocated at prepare
    std::atomic<bool> truePeakLimiter_{false};     // read at the next prepare only (a running session keeps its engine)
    std::uint32_t preparedRate_ = 0, preparedCh_ = 0, outCh_ = 0;
    Generation preparedGen_ = 0;
    bool havePrepared_ = false;
    ss::LatestMailbox<ss::FeatureSnapshot> features_;
    ss::LatestMailbox<ss::render::AudioVisualInput> visual_;
    std::atomic<ss::Ns> lastBlockNs_{0};
    std::atomic<std::uint64_t> blocks_{0}, onsets_{0};
};

}  // namespace ss::app
