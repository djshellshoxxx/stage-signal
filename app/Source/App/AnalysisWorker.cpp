#include "App/AnalysisWorker.h"
#include <algorithm>

namespace ss::app {

bool AnalysisWorker::ensurePrepared(const AudioBlockView& v) {
    if (havePrepared_ && v.rateHz == preparedRate_ && v.channels == preparedCh_ && v.generation == preparedGen_) return true;
    // Re-prepare on every device/rate/channel change (new Generation), off the audio thread.
    if (!analyzer_.prepare(v.rateHz, v.channels).ok()) { havePrepared_ = false; return false; }
    ss::dynamics::DynamicsConfig cfg;
    cfg.sampleRateHz = v.rateHz; cfg.inChannels = v.channels; cfg.monoToStereo = false;
    cfg.truePeakLimiter = truePeakLimiter_.load(std::memory_order_relaxed);
    auto info = dyn_.prepare(cfg);
    if (!info.ok()) { havePrepared_ = false; return false; }
    outCh_ = info.value().outChannels;
    out_.assign(std::size_t(1024) * std::max<std::uint32_t>(outCh_, 2), 0.f);   // ingress slots are <= 1024 frames
    preparedRate_ = v.rateHz; preparedCh_ = v.channels; preparedGen_ = v.generation;
    havePrepared_ = true;
    return true;
}

std::size_t AnalysisWorker::step() {
    std::size_t n = 0;
    AudioBlockView v;
    std::uint32_t slot = 0;
    while (in_.pop(v, slot)) {
        ++n;
        if (ensurePrepared(v)) {
            AudioBlockView raw = v;
            analyzer_.process(raw);
            // Dynamics sees the raw capture; its output feeds recording/broadcast only (never the host bus).
            const std::size_t need = std::size_t(v.frames) * outCh_;
            if (need <= out_.size() && dyn_.process(v.interleaved, out_.data(), v.frames)) {
                AudioBlockView proc{out_.data(), v.frames, outCh_, v.rateHz, v.firstSampleIndex, v.captureNs, v.flags, v.generation};
                for (auto* c : consumers_) c->onProcessedBlock(proc, raw);
            }
        }
        in_.release(slot);
        lastBlockNs_.store(clock_.nowMonotonic(), std::memory_order_relaxed);
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    if (n && havePrepared_) {
        const ss::FeatureSnapshot fs = analyzer_.snapshot();       // consumes the analyser's onset latch
        if (fs.onsetFired) onsets_.fetch_add(1, std::memory_order_relaxed);
        features_.publish(fs);
        ss::render::AudioVisualInput av;
        analyzer_.copySpectrum(av.spectrum);
        analyzer_.copyWaveform(av.waveform);
        visual_.publish(av);
    }
    return n;
}

ss::dynamics::DynamicsTelemetry AnalysisWorker::dynamicsTelemetry() const { return dyn_.telemetry(); }

}  // namespace ss::app
