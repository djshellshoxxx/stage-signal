// Fake / null adapter implementations for tests and the headless smoke (`--selftest`).
// Owner: shell-integration (the file); workers may ADD fakes in their own Engine/<Area>Fakes.h but must not
// change the behaviour of these. Everything here is header-only and JUCE-free.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "Engine/ICameraSource.h"
#include "Engine/IAudioSource.h"
#include "Engine/IDisplayProvider.h"
#include "Engine/IDurableFileSystem.h"
#include "Engine/IFileDialogs.h"
#include "Engine/ILogSink.h"
#include "Engine/IMidiPortProvider.h"
#include "Engine/IPacketSink.h"
#include "Engine/IPerfCounters.h"
#include "Engine/IProcessSupervisor.h"
#include "Engine/ISecretStoreAdapter.h"
#include "Engine/ISerialDmx.h"
#include "Engine/ITimestampMapper.h"
#include "Engine/IZipArchive.h"

namespace ss::app {

// Generates a sine on its own std::thread at real-time pace (not real-time safe itself; it only calls the sink
// the way a device callback would). pushManual() lets tests drive blocks deterministically instead.
class SineAudioSource final : public IAudioSource {
public:
    explicit SineAudioSource(double freqHz = 440.0, float amplitude = 0.5f, std::uint32_t rateHz = 48000,
                             std::uint32_t channels = 2, std::uint32_t blockFrames = 256)
        : freq_(freqHz), amp_(amplitude), rate_(rateHz), ch_(channels), block_(blockFrames) {}
    ~SineAudioSource() override { stop(); }

    void setSink(IAudioSink* s) override { sink_ = s; }
    ss::Status start() override {
        if (running_.exchange(true)) return ss::okStatus();
        if (sink_) sink_->streamOpened(format());
        state_ = AdapterState::Running;
        th_ = std::thread([this] { run(); });
        return ss::okStatus();
    }
    void stop() override {
        if (!running_.exchange(false)) return;
        if (th_.joinable()) th_.join();
        if (sink_) sink_->streamLost();
        state_ = AdapterState::Stopped;
    }
    AdapterHealth health() const override { return {state_.load(), {}}; }
    std::vector<AudioDeviceDesc> listDevices() override { return {{"Fake", "Sine generator", int(ch_), {44100.0, 48000.0}}}; }
    ss::Status selectDevice(const AudioDeviceDesc&) override { return ss::okStatus(); }
    AudioStreamFormat format() const override { return {rate_, ch_, block_, 0.0}; }
    bool hasDevice() const override { return true; }

    // Deterministic injection: one block of sine, advancing the phase. Caller thread plays "audio thread".
    void pushManual(std::uint32_t frames, ss::Ns nowNs) {
        fill(frames);
        if (sink_) sink_->pushBlock(buf_.data(), frames, ch_, rate_, nowNs);
    }

private:
    void fill(std::uint32_t frames) {
        buf_.resize(std::size_t(frames) * ch_);
        const double w = 2.0 * 3.14159265358979323846 * freq_ / rate_;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float v = amp_ * static_cast<float>(std::sin(phase_));
            phase_ += w;
            if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;
            for (std::uint32_t c = 0; c < ch_; ++c) buf_[std::size_t(i) * ch_ + c] = v;
        }
    }
    void run() {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(std::int64_t(1e9 * block_ / rate_));
        auto next = clock::now();
        while (running_.load()) {
            const auto now = clock::now();
            fill(block_);
            if (sink_) sink_->pushBlock(buf_.data(), block_, ch_, rate_,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
            next += period;
            std::this_thread::sleep_until(next);
        }
    }
    IAudioSink* sink_ = nullptr;
    double freq_; float amp_; std::uint32_t rate_, ch_, block_;
    double phase_ = 0.0;
    std::vector<float> buf_;
    std::atomic<bool> running_{false};
    std::atomic<AdapterState> state_{AdapterState::Stopped};
    std::thread th_;
};

// Source that never delivers: models "No audio device" (REQ-A02).
class NoAudioSource final : public IAudioSource {
public:
    void setSink(IAudioSink*) override {}
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Unavailable, "no audio device"}; }
    std::vector<AudioDeviceDesc> listDevices() override { return {}; }
    ss::Status selectDevice(const AudioDeviceDesc&) override {
        return ss::makeError(ss::ErrorCategory::DeviceLost, "no audio device");
    }
    AudioStreamFormat format() const override { return {}; }
    bool hasDevice() const override { return false; }
};

class FakeMidiPortProvider final : public IMidiPortProvider {
public:
    struct Out final : IMidiOutPort {
        ss::Status send(const std::uint8_t*, std::uint32_t) override { ++sent; return ss::okStatus(); }
        std::atomic<std::uint64_t> sent{0};
    };
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    std::vector<MidiPortInfo> listPorts() override { return {{"fake-in-1", "Fake MIDI In", true}, {"fake-out-1", "Fake MIDI Out", false}}; }
    ss::Result<std::uint32_t> openInput(const std::string&, IMidiInputSink* s) override { sink = s; return std::uint32_t{1}; }
    void closeInput(std::uint32_t) override { sink = nullptr; }
    ss::Result<IMidiOutPort*> openOutput(const std::string&) override { return static_cast<IMidiOutPort*>(&out); }
    void closeOutput(IMidiOutPort*) override {}
    IMidiInputSink* sink = nullptr;
    Out out;
};

class FakeDisplayProvider final : public IDisplayProvider {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    ss::router::Topology topology() override {
        std::lock_guard<std::mutex> g(m_);
        return displays_;
    }
    void setChangeCallback(std::function<void()> cb) override { cb_ = std::move(cb); }
    void script(ss::router::Topology t) { { std::lock_guard<std::mutex> g(m_); displays_ = std::move(t); } if (cb_) cb_(); }
    FakeDisplayProvider() {
        ss::router::PhysicalDisplay d; d.identity = "fake-display-main"; d.connector = "FAKE-1"; d.width = 1280; d.height = 720;
        d.primary = true; displays_.push_back(d);
    }
private:
    std::mutex m_;
    ss::router::Topology displays_;
    std::function<void()> cb_;
};

class FakeProcessSupervisor final : public IProcessSupervisor {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    FfmpegProbe probeFfmpeg(const std::string&) override { return {}; }   // found=false: honest, no encoder
    bool writeStdin(ss::broadcast::IProcess&, const std::uint8_t*, std::size_t) override { return false; }
    std::size_t liveChildren() const override { return 0; }
    std::unique_ptr<ss::broadcast::IProcess> create() override { return factory.create(); }
    ss::broadcast::FakeProcessFactory factory;
};

}  // namespace ss::app
