#include "App/AudioIngress.h"
#include <algorithm>
#include <cmath>

namespace ss::app {

AudioIngress::AudioIngress(ITimestampMapper& mapper, Config cfg)
    : mapper_(mapper), cfg_(cfg), slots_(cfg.slots), ready_(cfg.slots + 1), free_(cfg.slots + 1) {
    for (auto& s : slots_) s.samples.assign(std::size_t(cfg_.maxFramesPerSlot) * cfg_.maxChannels, 0.f);
    for (std::uint32_t i = 0; i < slots_.size(); ++i) free_.push(i);
}

void AudioIngress::streamOpened(const AudioStreamFormat& fmt) {
    rate_.store(fmt.rateHz, std::memory_order_relaxed);
    channels_.store(std::min(fmt.channels, cfg_.maxChannels), std::memory_order_relaxed);
    lost_.store(false, std::memory_order_release);
    requestedGen_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioIngress::streamLost() { lost_.store(true, std::memory_order_release); }

void AudioIngress::pushBlock(const float* in, std::uint32_t frames, std::uint32_t channels, std::uint32_t rateHz,
                             ss::Ns entryNs) {
    if (!in || frames == 0 || channels == 0) return;
    const Generation want = requestedGen_.load(std::memory_order_acquire);
    if (want != seenGen_) {              // new stream: restart the exact sample counter
        seenGen_ = want;
        sampleCounter_ = 0;
        mapper_.reset();
        pendingDiscontinuity_ = true;
    }
    const std::uint32_t outCh = std::min(channels, cfg_.maxChannels);
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = std::min(frames - done, cfg_.maxFramesPerSlot);
        std::uint32_t idx = 0;
        if (!free_.pop(idx)) {           // pool exhausted: drop, count, remember the gap
            dropped_.fetch_add(1, std::memory_order_relaxed);
            sampleCounter_ += n;         // the counter stays exact across the gap
            pendingDiscontinuity_ = true;
            done += n;
            continue;
        }
        Slot& s = slots_[idx];
        std::uint32_t flags = 0;
        bool silent = true;
        std::uint64_t bad = 0;
        for (std::uint32_t f = 0; f < n; ++f) {
            const float* src = in + std::size_t(done + f) * channels;
            float* dst = s.samples.data() + std::size_t(f) * outCh;
            for (std::uint32_t c = 0; c < outCh; ++c) {
                float v = src[c];
                if (!std::isfinite(v)) { v = 0.f; ++bad; }
                if (v > 0.f || v < 0.f) silent = false;
                dst[c] = v;
            }
        }
        if (bad) sanitised_.fetch_add(bad, std::memory_order_relaxed);
        if (silent) flags |= ss::kAudioSilent;
        if (pendingDiscontinuity_) { flags |= ss::kAudioDiscontinuity; pendingDiscontinuity_ = false; disc_.fetch_add(1, std::memory_order_relaxed); }
        const MappedTime mt = mapper_.map(sampleCounter_, rateHz, entryNs);
        if (mt.valid) flags |= ss::kAudioTimestampValid;
        s.frames = n; s.channels = outCh; s.rateHz = rateHz; s.flags = flags;
        s.firstSample = sampleCounter_; s.captureNs = mt.captureNs; s.gen = seenGen_;
        // ready_ holds at least as many entries as the pool has slots, so this cannot fail.
        ready_.push(idx);
        blocks_.fetch_add(1, std::memory_order_relaxed);
        frames_.fetch_add(n, std::memory_order_relaxed);
        sampleCounter_ += n;
        // The offset into the callback shifts capture time of later chunks; mapper works from sample index.
        done += n;
    }
}

bool AudioIngress::pop(AudioBlockView& v, std::uint32_t& slot) {
    std::uint32_t idx = 0;
    if (!ready_.pop(idx)) return false;
    const Slot& s = slots_[idx];
    v = AudioBlockView{s.samples.data(), s.frames, s.channels, s.rateHz, s.firstSample, s.captureNs, s.flags, s.gen};
    slot = idx;
    return true;
}

void AudioIngress::release(std::uint32_t slot) { free_.push(slot); }

AudioIngressStats AudioIngress::stats() const {
    AudioIngressStats st;
    st.blocks = blocks_.load(); st.frames = frames_.load(); st.droppedBlocks = dropped_.load();
    st.sanitisedSamples = sanitised_.load(); st.discontinuities = disc_.load();
    st.generation = requestedGen_.load(); st.rateHz = rate_.load(); st.channels = channels_.load();
    st.lost = lost_.load();
    return st;
}

}  // namespace ss::app
