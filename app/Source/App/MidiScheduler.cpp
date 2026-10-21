#include "App/MidiScheduler.h"
#include <algorithm>

namespace ss::app {

// One SPSC ring per input port: each port has exactly one OS producer thread and the MIDI-OUT thread as consumer.
struct MidiDriver::Ring final : IMidiInputSink {
    struct Msg { std::uint8_t b[3] = {0, 0, 0}; std::uint8_t n = 0; ss::Ns stamp = 0; };
    ss::SpscQueue<Msg> q{1024};
    void onMidiBytes(std::uint32_t, const std::uint8_t* bytes, std::uint32_t n, ss::Ns stampNs) override {
        if (n == 0 || n > 3) return;
        Msg m; m.n = static_cast<std::uint8_t>(n); m.stamp = stampNs;
        for (std::uint32_t i = 0; i < n; ++i) m.b[i] = bytes[i];
        q.push(m);                                      // full ring: newest dropped and counted by the queue
    }
};

struct MidiDriver::OpenIn {
    std::string identity;
    std::uint32_t portId = 0;
    ss::Generation gen = 0;
    std::unique_ptr<Ring> ring;
};

MidiDriver::MidiDriver(IMidiPortProvider& p, const ss::IClock& c) : provider_(p), clock_(c) {}

MidiDriver::~MidiDriver() {
    for (auto& o : open_) provider_.closeInput(o->portId);
}

void MidiDriver::post(EngineJob job) { std::lock_guard<std::mutex> g(m_); jobs_.push_back(std::move(job)); }
void MidiDriver::wantInput(const std::string& id) { std::lock_guard<std::mutex> g(m_); wanted_.insert(id); lastPoll_ = 0; }
void MidiDriver::unwantInput(const std::string& id) { std::lock_guard<std::mutex> g(m_); wanted_.erase(id); }
void MidiDriver::setActionSink(ActionSink s) { std::lock_guard<std::mutex> g(m_); actions_ = std::move(s); }

void MidiDriver::reconcile(ss::Ns now) {
    std::set<std::string> want;
    { std::lock_guard<std::mutex> g(m_); want = wanted_; }
    const auto ports = provider_.listPorts();
    auto present = [&](const std::string& id) {
        return std::any_of(ports.begin(), ports.end(), [&](const MidiPortInfo& p) { return p.isInput && p.identity == id; });
    };
    // Close ports that were unwanted or vanished (hot-plug loss): the engine learns about the loss.
    for (auto it = open_.begin(); it != open_.end();) {
        if (want.count((*it)->identity) == 0 || !present((*it)->identity)) {
            engine_.inputPortLost((*it)->portId, now);
            provider_.closeInput((*it)->portId);
            it = open_.erase(it);
        } else ++it;
    }
    for (const auto& id : want) {
        if (!present(id)) continue;
        if (std::any_of(open_.begin(), open_.end(), [&](const auto& o) { return o->identity == id; })) continue;
        auto o = std::make_unique<OpenIn>();
        o->identity = id; o->ring = std::make_unique<Ring>();
        auto r = provider_.openInput(id, o->ring.get());
        if (!r.ok()) continue;                          // retried at the next poll
        o->portId = r.value();
        std::string name = id;
        for (const auto& p : ports) if (p.isInput && p.identity == id) name = p.name;
        o->gen = engine_.openInputPort(o->portId, id, name, now);
        open_.push_back(std::move(o));
    }
    inputPorts_.store(static_cast<std::uint32_t>(open_.size()));
}

void MidiDriver::onTick(const MidiTickContext& ctx) {
    std::vector<EngineJob> jobs;
    ActionSink sink;
    { std::lock_guard<std::mutex> g(m_); jobs.swap(jobs_); sink = actions_; }
    for (auto& j : jobs) j(engine_);

    if (lastPoll_ == 0 || ctx.now - lastPoll_ >= ss::midi::kDevicePollPeriodNs) { lastPoll_ = ctx.now ? ctx.now : 1; reconcile(ctx.now); }

    std::uint64_t dropped = 0;
    for (auto& o : open_) {
        Ring::Msg m;
        int budget = 512;                                // bounded work per tick
        while (budget-- > 0 && o->ring->q.pop(m)) engine_.onBytes(o->portId, o->gen, m.b, m.n, m.stamp);
        dropped += o->ring->q.dropped();
    }
    inputDropped_.store(dropped, std::memory_order_relaxed);

    actionScratch_.clear();
    engine_.poll(ctx.now, actionScratch_);
    if (sink) for (const auto& a : actionScratch_) sink(a);

    if (ctx.outputEnabled && ctx.port) {
        outScratch_.clear();
        engine_.dueOutput(ctx.now, outScratch_);
        for (const auto& p : outScratch_) {
            if (p.midi.size == 0) continue;              // SysEx is disabled by default and not sent from here
            if (ctx.port->send(p.midi.bytes, p.midi.size).ok()) sent_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

MidiScheduler::MidiScheduler(const ss::IClock& clock, IMidiPortProvider& provider)
    : clock_(clock), provider_(provider), driver_(std::make_unique<MidiDriver>(provider, clock)) {
    handler_ = [d = driver_.get()](const MidiTickContext& c) { d->onTick(c); };
}

MidiScheduler::~MidiScheduler() = default;

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
    v.inputPorts = driver_->inputPorts();
    v.outputPorts = port_.load() ? 1u : 0u;
    v.messagesSent = driver_->messagesSent();
    v.inputDropped = driver_->inputDropped();
    v.note = v.outputEnabled ? (port_.load() ? "MIDI output enabled" : "MIDI output enabled, no output port open") : "MIDI output off";
}

}  // namespace ss::app
