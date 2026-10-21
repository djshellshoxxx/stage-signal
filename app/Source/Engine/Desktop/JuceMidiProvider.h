// IMidiPortProvider over juce::MidiInput / juce::MidiOutput (app-adapters 5). Hot-plug is observed by listPorts()
// (the MIDI scheduler polls it at most every 500 ms); open ports whose device vanished are reported through health().
// The input callback stamps IClock::now() and forwards 1..3 byte messages only. SysEx is dropped (and counted).
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "Engine/IMidiPortProvider.h"
#include "ss/base/time.hpp"

namespace ss::app {

class JuceMidiProvider final : public IMidiPortProvider {
public:
    explicit JuceMidiProvider(std::shared_ptr<ss::IClock> clock);
    ~JuceMidiProvider() override;

    ss::Status start() override;
    void stop() override;
    AdapterHealth health() const override;
    std::vector<MidiPortInfo> listPorts() override;
    ss::Result<std::uint32_t> openInput(const std::string& identity, IMidiInputSink* sink) override;
    void closeInput(std::uint32_t portId) override;
    ss::Result<IMidiOutPort*> openOutput(const std::string& identity) override;
    void closeOutput(IMidiOutPort* p) override;

    std::uint64_t sysexDropped() const { return sysexDropped_.load(); }
    std::size_t openInputCount() const;

private:
    struct In; struct Out;
    std::shared_ptr<ss::IClock> clock_;
    mutable std::mutex m_;
    std::vector<std::unique_ptr<In>> ins_;
    std::vector<std::unique_ptr<Out>> outs_;
    std::uint32_t nextId_ = 1;
    bool started_ = false;
    std::atomic<std::uint64_t> sysexDropped_{0};
    std::atomic<int> lostPorts_{0};
    std::string lastError_;
};

}  // namespace ss::app
