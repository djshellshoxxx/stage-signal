#include "Engine/Desktop/JuceMidiProvider.h"
#include <algorithm>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Engine/Desktop/DesktopCommon.h"

namespace ss::app {

struct JuceMidiProvider::In final : juce::MidiInputCallback {
    JuceMidiProvider* owner = nullptr;
    std::uint32_t portId = 0;
    std::string identity;
    std::atomic<IMidiInputSink*> sink{nullptr};
    std::unique_ptr<juce::MidiInput> dev;
    // OS MIDI thread: no allocation, no lock, no parsing.
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override {
        const auto* d = m.getRawData();
        const int n = m.getRawDataSize();
        if (n <= 0 || n > 3 || (d[0] == 0xF0)) { owner->sysexDropped_.fetch_add(1, std::memory_order_relaxed); return; }
        if (auto* s = sink.load(std::memory_order_acquire))
            s->onMidiBytes(portId, d, static_cast<std::uint32_t>(n), owner->clock_->nowMonotonic());
    }
};

struct JuceMidiProvider::Out final : IMidiOutPort {
    std::string identity;
    std::unique_ptr<juce::MidiOutput> dev;
    ss::Status send(const std::uint8_t* b, std::uint32_t n) override {
        if (!dev || n == 0 || n > 3) return desktop::err(ss::ErrorCategory::InvalidArgument, "bad MIDI message");
        dev->sendMessageNow(juce::MidiMessage(b, static_cast<int>(n)));
        return ss::okStatus();
    }
};

JuceMidiProvider::JuceMidiProvider(std::shared_ptr<ss::IClock> clock) : clock_(std::move(clock)) {}
JuceMidiProvider::~JuceMidiProvider() { stop(); }

ss::Status JuceMidiProvider::start() { std::lock_guard<std::mutex> g(m_); started_ = true; return ss::okStatus(); }

void JuceMidiProvider::stop() {
    std::vector<std::unique_ptr<In>> ins;
    std::vector<std::unique_ptr<Out>> outs;
    {
        std::lock_guard<std::mutex> g(m_);
        started_ = false;
        ins.swap(ins_); outs.swap(outs_);
    }
    for (auto& i : ins) { i->sink.store(nullptr); if (i->dev) i->dev->stop(); }
    ins.clear(); outs.clear();                         // destroys devices outside the lock
}

std::size_t JuceMidiProvider::openInputCount() const { std::lock_guard<std::mutex> g(m_); return ins_.size(); }

std::vector<MidiPortInfo> JuceMidiProvider::listPorts() {
    std::vector<MidiPortInfo> v;
    for (const auto& d : juce::MidiInput::getAvailableDevices()) v.push_back({d.identifier.toStdString(), d.name.toStdString(), true});
    for (const auto& d : juce::MidiOutput::getAvailableDevices()) v.push_back({d.identifier.toStdString(), d.name.toStdString(), false});
    // Hot-plug bookkeeping for health(): open input whose identity is no longer enumerated.
    int lost = 0;
    {
        std::lock_guard<std::mutex> g(m_);
        for (const auto& i : ins_)
            if (std::none_of(v.begin(), v.end(), [&](const MidiPortInfo& p) { return p.isInput && p.identity == i->identity; })) ++lost;
    }
    lostPorts_.store(lost);
    return v;
}

AdapterHealth JuceMidiProvider::health() const {
    std::lock_guard<std::mutex> g(m_);
    if (!started_) return {AdapterState::Stopped, {}};
    if (lostPorts_.load() > 0) return {AdapterState::Degraded, "a MIDI input device was disconnected"};
    return {AdapterState::Running, lastError_};
}

ss::Result<std::uint32_t> JuceMidiProvider::openInput(const std::string& identity, IMidiInputSink* sink) {
    if (!sink) return desktop::err(ss::ErrorCategory::InvalidArgument, "no sink");
    auto in = std::make_unique<In>();
    in->owner = this; in->identity = identity; in->sink.store(sink);
    {
        std::lock_guard<std::mutex> g(m_);
        if (!started_) return desktop::err(ss::ErrorCategory::UnsupportedCapability, "MIDI provider not started");
        in->portId = nextId_++;
    }
    in->dev = juce::MidiInput::openDevice(juce::String(identity), in.get());
    if (!in->dev) return desktop::err(ss::ErrorCategory::DeviceLost, "MIDI input could not be opened", ss::Recoverability::Retryable);
    in->dev->start();
    const std::uint32_t id = in->portId;
    std::lock_guard<std::mutex> g(m_);
    ins_.push_back(std::move(in));
    return id;
}

void JuceMidiProvider::closeInput(std::uint32_t portId) {
    std::unique_ptr<In> victim;
    {
        std::lock_guard<std::mutex> g(m_);
        auto it = std::find_if(ins_.begin(), ins_.end(), [&](const auto& p) { return p->portId == portId; });
        if (it == ins_.end()) return;
        victim = std::move(*it); ins_.erase(it);
    }
    victim->sink.store(nullptr);
    if (victim->dev) victim->dev->stop();
}

ss::Result<IMidiOutPort*> JuceMidiProvider::openOutput(const std::string& identity) {
    auto out = std::make_unique<Out>();
    out->identity = identity;
    out->dev = juce::MidiOutput::openDevice(juce::String(identity));
    if (!out->dev) return desktop::err(ss::ErrorCategory::DeviceLost, "MIDI output could not be opened", ss::Recoverability::Retryable);
    std::lock_guard<std::mutex> g(m_);
    if (!started_) return desktop::err(ss::ErrorCategory::UnsupportedCapability, "MIDI provider not started");
    IMidiOutPort* raw = out.get();
    outs_.push_back(std::move(out));
    return raw;
}

void JuceMidiProvider::closeOutput(IMidiOutPort* p) {
    std::unique_ptr<Out> victim;
    {
        std::lock_guard<std::mutex> g(m_);
        auto it = std::find_if(outs_.begin(), outs_.end(), [&](const auto& o) { return o.get() == p; });
        if (it == outs_.end()) return;
        victim = std::move(*it); outs_.erase(it);
    }
}

}  // namespace ss::app
