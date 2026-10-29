// IAudioSource over juce::AudioDeviceManager (app-adapters 3.1). Input only. The device callback interleaves into a
// pre-allocated buffer and calls IAudioSink::pushBlock: no allocation, lock, log or file access there.
// Loss/return: audioDeviceStopped/Error mark the stream lost; a supervisor thread retries restartLastAudioDevice
// every 2 s (30 tries) then every 10 s, and also retries opening when no device existed at start.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "Engine/IAudioSource.h"
#include "ss/base/time.hpp"

namespace ss::app {

class LinearFitTimestampMapper;

class JuceAudioSource final : public IAudioSource {
public:
    JuceAudioSource(std::shared_ptr<ss::IClock> clock, std::shared_ptr<LinearFitTimestampMapper> mapper);
    ~JuceAudioSource() override;

    ss::Status start() override;
    void stop() override;
    AdapterHealth health() const override;
    void setSink(IAudioSink* sink) override { sink_.store(sink, std::memory_order_release); }
    std::vector<AudioDeviceDesc> listDevices() override;
    ss::Status selectDevice(const AudioDeviceDesc& d) override;
    AudioStreamFormat format() const override;
    bool hasDevice() const override { return deviceOpen_.load(std::memory_order_acquire); }

    // Test seam: simulates the callbacks a backend would deliver (no hardware needed).
    struct Impl;
    Impl* impl() { return impl_.get(); }
    void simulateBlock(const float* const* ch, int numCh, int numSamples);
    void simulateStopped();
    void simulateStarted(double rate, int bufferSize);

private:
    friend struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<ss::IClock> clock_;
    std::shared_ptr<LinearFitTimestampMapper> mapper_;
    std::atomic<IAudioSink*> sink_{nullptr};
    std::atomic<bool> deviceOpen_{false};
};

}  // namespace ss::app
