#include "App/MidiScheduler.h"

namespace ss::app {

void MidiScheduler::setTickHandler(TickHandler h) {
    std::lock_guard<std::mutex> g(handlerMutex_);
    handler_ = std::move(h);
}

void MidiScheduler::tick() {
    ticks_.fetch_add(1, std::memory_order_relaxed);
    TickHandler h;
    { std::lock_guard<std::mutex> g(handlerMutex_); h = handler_; }
    if (!h) return;
    const bool on = outputEnabled();
    MidiTickContext ctx{clock_.nowMonotonic(), on, on ? port_.load(std::memory_order_acquire) : nullptr};
    h(ctx);
}

void MidiScheduler::fillView(MidiView& v) const {
    v.outputEnabled = outputEnabled();
    v.ticks = ticks_.load();
    v.note = v.outputEnabled ? "MIDI output enabled" : "MIDI output off";
    (void) provider_;
}

}  // namespace ss::app
