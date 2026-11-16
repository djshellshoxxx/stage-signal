// Contract: audio ingress (app-shell 4.2, APP-SH-03, APP-AD-01/03). Owner: shell-integration.
// The one place where audio enters the app. pushBlock() is the audio-thread entry: it copies, sanitises (NaN/Inf
// -> 0, counted), stamps an exact running sample index and a mapped capture time, and enqueues a pooled slot.
// No allocation, lock, file, socket, UI or log call. If the pool or queue is full the block is dropped, counted,
// and the next accepted block carries kAudioDiscontinuity (never waits).
// The ANALYSIS thread is the single consumer: pop() / release().
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "Engine/IAudioSource.h"
#include "Engine/ITimestampMapper.h"
#include "ss/base/queue.hpp"
#include "ss/base/types.hpp"

namespace ss::app {

struct AudioIngressStats {
    std::uint64_t blocks = 0, frames = 0, droppedBlocks = 0, sanitisedSamples = 0, discontinuities = 0;
    Generation generation = 0;
    std::uint32_t rateHz = 0, channels = 0;
    bool lost = true;                     // true until a stream is opened; true again after streamLost()
};

class AudioIngress final : public IAudioSink {
public:
    struct Config {
        std::size_t slots = 192;                 // ~1 s at 256-frame blocks / 48 kHz (spec baseline 250 ms)
        std::uint32_t maxFramesPerSlot = 1024;   // larger callbacks are split into several slots
        std::uint32_t maxChannels = 2;           // mono or stereo route; extra input channels are ignored
    };
    AudioIngress(ITimestampMapper& mapper, Config cfg);
    explicit AudioIngress(ITimestampMapper& mapper) : AudioIngress(mapper, Config{}) {}

    // ---- IAudioSink -------------------------------------------------------------------------
    void pushBlock(const float* interleaved, std::uint32_t frames, std::uint32_t channels, std::uint32_t rateHz,
                   ss::Ns callbackEntryNs) override;                   // AUDIO thread
    void streamOpened(const AudioStreamFormat& fmt) override;           // any non-audio thread
    void streamLost() override;

    // ---- consumer (ANALYSIS thread) ---------------------------------------------------------
    // The view stays valid until release(slot). Returns false when nothing is queued.
    bool pop(AudioBlockView& view, std::uint32_t& slot);
    void release(std::uint32_t slot);

    AudioIngressStats stats() const;                                    // any thread

private:
    struct Slot {
        std::vector<float> samples;
        std::uint32_t frames = 0, channels = 0, rateHz = 0, flags = 0;
        std::uint64_t firstSample = 0;
        ss::Ns captureNs = 0;
        Generation gen = 0;
    };
    ITimestampMapper& mapper_;
    Config cfg_;
    std::vector<Slot> slots_;
    ss::SpscQueue<std::uint32_t> ready_, free_;
    // audio-thread state
    std::uint64_t sampleCounter_ = 0;
    Generation seenGen_ = 0;
    bool pendingDiscontinuity_ = false;
    // shared
    std::atomic<Generation> requestedGen_{0};
    std::atomic<std::uint32_t> rate_{0}, channels_{0};
    std::atomic<bool> lost_{true};
    std::atomic<std::uint64_t> blocks_{0}, frames_{0}, dropped_{0}, sanitised_{0}, disc_{0};
};

}  // namespace ss::app
