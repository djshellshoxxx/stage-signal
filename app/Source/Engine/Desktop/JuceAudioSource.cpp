#include "Engine/Desktop/JuceAudioSource.h"
#include <algorithm>
#include <condition_variable>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Engine/Desktop/DesktopCommon.h"
#include "Engine/Desktop/TimestampMapperLinear.h"

namespace ss::app {

struct JuceAudioSource::Impl final : juce::AudioIODeviceCallback {
    explicit Impl(JuceAudioSource& o) : owner(o) {}

    JuceAudioSource& owner;
    std::mutex lifeMutex;                       // start/stop/select/open only; never taken by callbacks
    std::unique_ptr<juce::AudioDeviceManager> mgr;
    std::atomic<bool> started{false};
    std::string wantType, wantName;             // explicit selection (never replaced by another device)

    mutable std::mutex healthMutex;             // health text only
    AdapterState state = AdapterState::Stopped;
    std::string lastError;

    std::atomic<std::uint32_t> rate{0}, channels{0}, buffer{0};
    std::atomic<double> latencyMs{0.0};
    std::atomic<bool> lost{false};
    std::vector<float> scratch;                 // sized in start of stream, used only by the callback
    std::atomic<std::uint64_t> sampleCounter{0};

    std::thread supervisor;
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic<bool> stopSupervisor{false};

    void setState(AdapterState s, const std::string& e) {
        std::lock_guard<std::mutex> g(healthMutex);
        state = s; lastError = desktop::clean(e);
    }

    // ---- juce::AudioIODeviceCallback ----
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override {
        const auto bs = static_cast<std::size_t>(std::max(1, d->getCurrentBufferSizeSamples()));
        const std::size_t need = bs * 2;
        if (scratch.size() < need) scratch.assign(need, 0.0f);      // device thread, before the stream runs
        const double sr = d->getCurrentSampleRate();
        rate.store(static_cast<std::uint32_t>(sr + 0.5)); buffer.store(static_cast<std::uint32_t>(bs));
        const int inCh = d->getActiveInputChannels().countNumberOfSetBits();
        channels.store(static_cast<std::uint32_t>(std::min(2, inCh)));
        const double lat = sr > 0 ? 1000.0 * d->getInputLatencyInSamples() / sr : 0.0;
        latencyMs.store(lat);
        if (owner.mapper_) {
            owner.mapper_->setLatencyNs(static_cast<ss::Ns>((lat + (sr > 0 ? 1000.0 * static_cast<double>(bs) / sr : 0.0)) * 1e6));
            owner.mapper_->reset();
        }
        sampleCounter.store(0);
        lost.store(false);
        owner.deviceOpen_.store(true, std::memory_order_release);
        const bool rateOk = std::abs(sr - 44100.0) < 1.0 || std::abs(sr - 48000.0) < 1.0;
        setState(inCh > 0 ? (rateOk ? AdapterState::Running : AdapterState::Degraded) : AdapterState::Degraded,
                 inCh == 0 ? "device has no input channels" : (rateOk ? "" : "sample rate is not 44.1 or 48 kHz"));
        if (auto* s = owner.sink_.load()) s->streamOpened(owner.format());
    }

    void audioDeviceStopped() override { markLost("audio device stopped"); }
    void audioDeviceError(const juce::String& m) override { markLost(m.toStdString()); }

    void markLost(const std::string& why) {
        if (!started) return;
        const bool was = lost.exchange(true);
        owner.deviceOpen_.store(false, std::memory_order_release);
        setState(AdapterState::Degraded, "audio device lost: " + why);
        if (!was) if (auto* s = owner.sink_.load()) s->streamLost();
        wake.notify_all();
    }

    void audioDeviceIOCallbackWithContext(const float* const* in, int nIn, float* const* out, int nOut, int n,
                                          const juce::AudioIODeviceCallbackContext&) override {
        for (int c = 0; c < nOut; ++c) if (out[c]) std::fill(out[c], out[c] + n, 0.0f);
        block(in, nIn, n);
    }

    // Audio thread. Chunked so a larger-than-expected callback never reallocates.
    void block(const float* const* in, int nIn, int n) {
        auto* sink = owner.sink_.load(std::memory_order_acquire);
        const int ch = std::min(2, nIn);
        if (!sink || ch <= 0 || n <= 0 || rate.load(std::memory_order_relaxed) == 0) return;
        const ss::Ns entry = owner.clock_->nowMonotonic();
        const int cap = static_cast<int>(scratch.size()) / ch;
        for (int off = 0; off < n; off += cap) {
            const int m = std::min(cap, n - off);
            float* dst = scratch.data();
            for (int i = 0; i < m; ++i)
                for (int c = 0; c < ch; ++c) *dst++ = in[c] ? in[c][off + i] : 0.0f;
            sink->pushBlock(scratch.data(), static_cast<std::uint32_t>(m), static_cast<std::uint32_t>(ch),
                            rate.load(std::memory_order_relaxed), entry);
        }
        sampleCounter.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    }

    // ---- open / retry (lifeMutex held by caller) ----
    ss::Status openLocked() {
        if (!mgr) mgr = std::make_unique<juce::AudioDeviceManager>();
        mgr->removeAudioCallback(this);
        juce::String e;
        if (wantName.empty()) {
            e = mgr->initialise(2, 0, nullptr, true, {}, nullptr);
        } else {
            e = mgr->initialise(2, 0, nullptr, false, {}, nullptr);
            if (e.isEmpty()) {
                mgr->setCurrentAudioDeviceType(juce::String(wantType), true);
                juce::AudioDeviceManager::AudioDeviceSetup s;
                mgr->getAudioDeviceSetup(s);
                s.inputDeviceName = juce::String(wantName); s.outputDeviceName = {};
                s.useDefaultInputChannels = true; s.useDefaultOutputChannels = false;
                e = mgr->setAudioDeviceSetup(s, true);
            }
        }
        if (e.isNotEmpty()) return desktop::err(ss::ErrorCategory::DeviceLost, e.toStdString(), ss::Recoverability::Retryable);
        if (mgr->getCurrentAudioDevice() == nullptr)
            return desktop::err(ss::ErrorCategory::DeviceLost, "no audio input device", ss::Recoverability::Retryable);
        mgr->addAudioCallback(this);                 // triggers audioDeviceAboutToStart
        return ss::okStatus();
    }

    void supervise() {
        int attempts = 0;
        while (!stopSupervisor.load()) {
            {
                std::unique_lock<std::mutex> g(wakeMutex);
                wake.wait_for(g, std::chrono::milliseconds(attempts < 30 ? 2000 : 10000));
            }
            if (stopSupervisor.load()) break;
            if (owner.deviceOpen_.load() && !lost.load()) { attempts = 0; continue; }
            ++attempts;
            std::lock_guard<std::mutex> g(lifeMutex);
            if (!started) break;
            if (mgr && mgr->getCurrentAudioDevice() != nullptr && lost.load()) mgr->restartLastAudioDevice();
            if (!owner.deviceOpen_.load()) {
                auto st = openLocked();
                if (!st.ok()) setState(AdapterState::Unavailable, st.error().detail);
            }
        }
    }
};

JuceAudioSource::JuceAudioSource(std::shared_ptr<ss::IClock> clock, std::shared_ptr<LinearFitTimestampMapper> mapper)
    : impl_(std::make_unique<Impl>(*this)), clock_(std::move(clock)), mapper_(std::move(mapper)) {}

JuceAudioSource::~JuceAudioSource() { stop(); }

ss::Status JuceAudioSource::start() {
    {
        std::lock_guard<std::mutex> g(impl_->lifeMutex);
        if (impl_->started) return impl_->state == AdapterState::Running ? ss::okStatus()
                                   : ss::Status(desktop::err(ss::ErrorCategory::DeviceLost, "no audio input device", ss::Recoverability::Retryable));
        impl_->started = true;
        impl_->stopSupervisor.store(false);
        impl_->setState(AdapterState::Starting, "");
    }
    ss::Status st = ss::okStatus();
    {
        std::lock_guard<std::mutex> g(impl_->lifeMutex);
        st = impl_->openLocked();
        if (!st.ok()) impl_->setState(AdapterState::Unavailable, st.error().detail);
    }
    impl_->supervisor = std::thread([this] { impl_->supervise(); });
    return st;
}

void JuceAudioSource::stop() {
    {
        std::lock_guard<std::mutex> g(impl_->lifeMutex);
        if (!impl_->started) return;
        impl_->started = false;
    }
    impl_->stopSupervisor.store(true);
    impl_->wake.notify_all();
    if (impl_->supervisor.joinable()) impl_->supervisor.join();
    std::lock_guard<std::mutex> g(impl_->lifeMutex);
    if (impl_->mgr) { impl_->mgr->removeAudioCallback(impl_.get()); impl_->mgr->closeAudioDevice(); impl_->mgr.reset(); }
    deviceOpen_.store(false);
    impl_->setState(AdapterState::Stopped, "");
}

AdapterHealth JuceAudioSource::health() const {
    std::lock_guard<std::mutex> g(impl_->healthMutex);
    return {impl_->state, impl_->lastError};
}

AudioStreamFormat JuceAudioSource::format() const {
    AudioStreamFormat f;
    f.rateHz = impl_->rate.load(); f.channels = impl_->channels.load(); f.bufferFrames = impl_->buffer.load();
    f.inputLatencyMs = impl_->latencyMs.load();
    return f;
}

std::vector<AudioDeviceDesc> JuceAudioSource::listDevices() {
    std::vector<AudioDeviceDesc> out;
    juce::AudioDeviceManager tmp;
    for (auto* type : tmp.getAvailableDeviceTypes()) {
        type->scanForDevices();
        for (const auto& name : type->getDeviceNames(true)) {
            AudioDeviceDesc d;
            d.typeName = type->getTypeName().toStdString(); d.deviceName = name.toStdString();
            std::unique_ptr<juce::AudioIODevice> dev(type->createDevice({}, name));
            if (dev) {
                d.inputChannels = dev->getInputChannelNames().size();
                for (double r : dev->getAvailableSampleRates()) d.sampleRates.push_back(r);
            }
            out.push_back(std::move(d));
        }
    }
    return out;
}

ss::Status JuceAudioSource::selectDevice(const AudioDeviceDesc& d) {
    if (d.deviceName.empty())
        return desktop::err(ss::ErrorCategory::InvalidArgument, "no device name");
    std::lock_guard<std::mutex> g(impl_->lifeMutex);
    impl_->wantType = d.typeName; impl_->wantName = d.deviceName;
    if (!impl_->started) return ss::okStatus();
    auto st = impl_->openLocked();                      // never substitutes another device
    if (!st.ok()) impl_->setState(AdapterState::Unavailable, "saved device not found");
    return st;
}

void JuceAudioSource::simulateStarted(double rate, int bufferSize) {
    impl_->started = true;
    impl_->scratch.assign(static_cast<std::size_t>(bufferSize) * 2, 0.0f);
    impl_->rate.store(static_cast<std::uint32_t>(rate)); impl_->buffer.store(static_cast<std::uint32_t>(bufferSize));
    impl_->channels.store(2); impl_->lost.store(false);
    if (mapper_) mapper_->reset();
    deviceOpen_.store(true);
    impl_->setState(AdapterState::Running, "");
    if (auto* s = sink_.load()) s->streamOpened(format());
}
void JuceAudioSource::simulateBlock(const float* const* ch, int numCh, int numSamples) { impl_->block(ch, numCh, numSamples); }
void JuceAudioSource::simulateStopped() { impl_->markLost("simulated"); }

}  // namespace ss::app
