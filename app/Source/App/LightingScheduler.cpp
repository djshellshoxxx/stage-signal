#include "App/LightingScheduler.h"

namespace ss::app {

LightingScheduler::LightingScheduler(const ss::IClock& clock, IPacketSink& realSink, BlackoutLatch& latch,
                                     const ss::LatestMailbox<ss::ShowSnapshotLite>& show,
                                     const ss::LatestMailbox<ss::FeatureSnapshot>& features)
    : clock_(clock), real_(realSink), latch_(latch), show_(show), features_(features) {
    driver_ = std::make_unique<LightingDriver>(real_, [this](const std::string& why) { noteRefused(why); });
    handler_ = [d = driver_.get()](const LightingTickContext& c) { d->onTick(c); };
}

void LightingScheduler::noteRefused(const std::string& why) {
    armed_.store(false, std::memory_order_release);       // an engine that is not armed must not leave the gate armed
    std::lock_guard<std::mutex> g(noteMutex_);
    faultNote_ = why;
}

void LightingScheduler::setTickHandler(TickHandler h) {
    std::lock_guard<std::mutex> g(handlerMutex_);
    handler_ = std::move(h);
}

ss::Status LightingScheduler::arm() {
    if (!real_.connected())
        return ss::makeError(ss::ErrorCategory::DeviceLost, "no lighting transport", ss::Recoverability::OperatorAction);
    { std::lock_guard<std::mutex> g(noteMutex_); faultNote_.clear(); }
    armed_.store(true, std::memory_order_release);
    return ss::okStatus();
}

void LightingScheduler::disarm() { armed_.store(false, std::memory_order_release); }

void LightingScheduler::tick() {
    ticks_.fetch_add(1, std::memory_order_relaxed);
    if (latch_.consumeDisarm()) disarm();            // safety-critical: does not wait for the CONTROL thread
    ss::ShowSnapshotLite lite;
    ss::FeatureSnapshot feat;
    show_.read(lite);
    features_.read(feat);
    const bool isArmed = armed();
    if (!isArmed) heldDisarmed_.fetch_add(1, std::memory_order_relaxed);
    const bool blackout = lite.lightingBlackout || latch_.anyLighting();
    blackout_.store(blackout, std::memory_order_relaxed);
    TickHandler h;
    { std::lock_guard<std::mutex> g(handlerMutex_); h = handler_; }
    if (!h) return;
    ss::lighting::IDatagramSink& sink = isArmed ? static_cast<ss::lighting::IDatagramSink&>(real_)
                                                : static_cast<ss::lighting::IDatagramSink&>(null_);
    LightingTickContext ctx{clock_.nowMonotonic(), isArmed, blackout, lite, feat, sink};
    h(ctx);
}

void LightingScheduler::fillView(LightingView& v) const {
    std::string fault;
    { std::lock_guard<std::mutex> g(noteMutex_); fault = faultNote_; }
    v.state = armed() ? LightingUiState::Armed : (fault.empty() ? LightingUiState::Disarmed : LightingUiState::Fault);
    v.engineArmed = driver_->engineState() == ss::lighting::EngineState::Armed;
    v.datagramsReal = driver_->realDatagrams(); v.datagramsMock = driver_->mockDatagrams(); v.faults = driver_->faults();
    v.transportConnected = real_.connected();
    v.blackout = blackout_.load(std::memory_order_relaxed);
    v.ticks = ticks_.load(); v.ticksHeldDisarmed = heldDisarmed_.load();
    v.framesSent = driver_->realDatagrams();
    v.note = armed() ? "Armed" : (fault.empty() ? "Disarmed, press Arm" : "Disarmed: " + fault);
}


// ---- LightingDriver -------------------------------------------------------------------------------------------------
ss::Status LightingDriver::GateSink::send(const ss::lighting::Datagram& d) {
    if (!open.load(std::memory_order_acquire))
        return ss::makeError(ss::ErrorCategory::DeviceLost, "lighting gate closed", ss::Recoverability::OperatorAction);
    return real.send(d);
}

LightingDriver::LightingDriver(ss::lighting::IDatagramSink& real, Refused refused)
    : gate_(real), engine_(&gate_), refused_(std::move(refused)) {}

void LightingDriver::post(EngineJob job) {
    std::lock_guard<std::mutex> g(jobsMutex_);
    jobs_.push_back(std::move(job));
}

void LightingDriver::onTick(const LightingTickContext& ctx) {
    using ss::lighting::EngineState;
    {
        std::vector<EngineJob> jobs;
        { std::lock_guard<std::mutex> g(jobsMutex_); jobs.swap(jobs_); }
        for (auto& j : jobs) j(engine_);
    }
    const Ns now = ctx.now;
    if (ctx.armed && engine_.state() != EngineState::Armed) {
        gate_.open.store(true, std::memory_order_release);
        auto st = engine_.arm();
        if (!st.ok()) { gate_.open.store(false, std::memory_order_release); ++faults_; if (refused_) refused_(st.error().detail); }
    } else if (!ctx.armed && engine_.state() == EngineState::Armed) {
        gate_.open.store(true, std::memory_order_release);      // safe frames + protocol termination are part of Disarm
        engine_.disarm(now);
        gate_.open.store(false, std::memory_order_release);
    }

    ss::lighting::LightingInputs in;
    in.purpose = ss::lighting::Purpose::Live;
    in.generation = engine_.generation();
    in.resolve.lightingBlackout = ctx.blackout;
    in.resolve.strobingDisabled = ctx.show.strobingDisabled;
    in.resolve.audio = ctx.features;
    const auto& r = engine_.tick(now, in);
    engineState_.store(engine_.state(), std::memory_order_relaxed);
    if (r.transmitted) {
        if (r.toRealSink) realDatagrams_.fetch_add(r.datagrams, std::memory_order_relaxed);
        else mockDatagrams_.fetch_add(r.datagrams, std::memory_order_relaxed);
    }
    if (engine_.state() != EngineState::Armed) {
        gate_.open.store(false, std::memory_order_release);
        if (ctx.armed) {                                        // fault dropped us out of Armed: surface it, require a new Arm
            ++faults_;
            std::string why = "transport fault";
            for (const auto& e : engine_.drainEvents()) if (e.kind == ss::lighting::EventKind::TransportFault) why = e.detail;
            if (refused_) refused_(why);
        }
    }
}

}  // namespace ss::app
