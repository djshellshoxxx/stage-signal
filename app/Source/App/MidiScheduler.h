// Contract: MIDI-OUT thread body (app-shell 4.1, step 11, APP-AD-06). Owner: engine-adapters-desktop.
// Output is DISABLED by default and after every start: the handler receives a null output port until the
// operator enables it (AppCommandKind::EnableMidiOutput). The handler drives ss::midi::MidiEngine::dueOutput()
// and writes to ctx.port; this commit ships only the scheduling and the enable gate.
// MIDI-IN is not a thread of ours: the provider's callback pushes into a SPSC ring owned by the handler's owner.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include "App/Views/MidiView.h"
#include "Engine/IMidiPortProvider.h"
#include "ss/base/time.hpp"

namespace ss::app {

struct MidiTickContext {
    ss::Ns now = 0;
    bool outputEnabled = false;
    IMidiOutPort* port = nullptr;       // nullptr while disabled
};

class MidiScheduler {
public:
    using TickHandler = std::function<void(const MidiTickContext&)>;
    MidiScheduler(const ss::IClock& clock, IMidiPortProvider& provider) : clock_(clock), provider_(provider) {}

    void setTickHandler(TickHandler h);
    void setOutputPort(IMidiOutPort* p) { port_.store(p, std::memory_order_release); }   // set by the opener
    void enableOutput() { enabled_.store(true, std::memory_order_release); }
    void disableOutput() { enabled_.store(false, std::memory_order_release); }           // idempotent
    bool outputEnabled() const { return enabled_.load(std::memory_order_acquire); }

    void tick();                         // MIDI-OUT thread (or tests); cadence 2 ms in the controller
    void fillView(MidiView& v) const;

private:
    const ss::IClock& clock_;
    IMidiPortProvider& provider_;
    std::atomic<bool> enabled_{false};
    std::atomic<IMidiOutPort*> port_{nullptr};
    std::mutex handlerMutex_;
    TickHandler handler_;
    std::atomic<std::uint64_t> ticks_{0};
};

}  // namespace ss::app
