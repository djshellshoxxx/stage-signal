// Contract: MIDI-OUT thread body (app-shell 4.1, step 11, APP-AD-06). Owner: engine-adapters-desktop.
// Output is DISABLED by default and after every start: the handler receives a null output port until the
// operator enables it (AppCommandKind::EnableMidiOutput).
// The scheduler owns a MidiDriver installed as the default tick handler. It owns the core ss::midi::MidiEngine and,
// on the MIDI-OUT thread: opens/reopens wanted input ports (hot-plug poll <= 500 ms), drains one SPSC ring per input
// port into MidiEngine::onBytes, polls produced Actions into the action sink, and (only while output is enabled)
// writes MidiEngine::dueOutput() packets to the output port. MIDI-IN is not a thread of ours: the provider's OS
// callback only pushes raw bytes into the per-port ring.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ss/base/queue.hpp"
#include "ss/midi/engine.hpp"
#include "App/Views/MidiView.h"
#include "Engine/IMidiPortProvider.h"
#include "ss/base/time.hpp"

namespace ss::app {

struct MidiTickContext {
    ss::Ns now = 0;
    bool outputEnabled = false;
    IMidiOutPort* port = nullptr;       // nullptr while disabled
};

class MidiDriver {
public:
    using ActionSink = std::function<void(const ss::Action&)>;
    using EngineJob = std::function<void(ss::midi::MidiEngine&)>;
    MidiDriver(IMidiPortProvider& provider, const ss::IClock& clock);
    ~MidiDriver();
    void post(EngineJob job);                          // any thread; runs on the MIDI-OUT thread at the next tick
    void wantInput(const std::string& identity);       // any thread: open it now and reopen after hot-plug loss
    void unwantInput(const std::string& identity);
    void setActionSink(ActionSink s);                  // any thread
    void onTick(const MidiTickContext& ctx);           // MIDI-OUT thread
    std::uint32_t inputPorts() const { return inputPorts_.load(); }
    std::uint64_t messagesSent() const { return sent_.load(); }
    std::uint64_t inputDropped() const { return inputDropped_.load(); }
    ss::midi::MidiEngine& engine() { return engine_; }  // MIDI-OUT thread or tests with the loop stopped
private:
    struct Ring;
    struct OpenIn;
    void reconcile(ss::Ns now);
    IMidiPortProvider& provider_;
    const ss::IClock& clock_;
    ss::midi::MidiEngine engine_;
    std::mutex m_;
    std::vector<EngineJob> jobs_;
    std::set<std::string> wanted_;
    ActionSink actions_;
    std::vector<std::unique_ptr<OpenIn>> open_;        // MIDI-OUT thread only
    std::vector<ss::Action> actionScratch_;
    std::vector<ss::midi::OutPacket> outScratch_;
    ss::Ns lastPoll_ = 0;
    std::atomic<std::uint32_t> inputPorts_{0};
    std::atomic<std::uint64_t> sent_{0}, inputDropped_{0};
};

class MidiScheduler {
public:
    using TickHandler = std::function<void(const MidiTickContext&)>;
    MidiScheduler(const ss::IClock& clock, IMidiPortProvider& provider);
    ~MidiScheduler();

    void setTickHandler(TickHandler h);
    void setOutputPort(IMidiOutPort* p) { port_.store(p, std::memory_order_release); }   // set by the opener
    void enableOutput() { enabled_.store(true, std::memory_order_release); }
    void disableOutput() { enabled_.store(false, std::memory_order_release); }           // idempotent
    bool outputEnabled() const { return enabled_.load(std::memory_order_acquire); }

    void tick();                         // MIDI-OUT thread (or tests); cadence 2 ms in the controller
    void fillView(MidiView& v) const;
    MidiDriver& driver() { return *driver_; }

private:
    const ss::IClock& clock_;
    IMidiPortProvider& provider_;
    std::atomic<bool> enabled_{false};
    std::atomic<IMidiOutPort*> port_{nullptr};
    std::mutex handlerMutex_;
    TickHandler handler_;
    std::atomic<std::uint64_t> ticks_{0};
    std::unique_ptr<MidiDriver> driver_;
};

}  // namespace ss::app
