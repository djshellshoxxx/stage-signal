#include "App/LightingScheduler.h"

namespace ss::app {

LightingScheduler::LightingScheduler(const ss::IClock& clock, IPacketSink& realSink, BlackoutLatch& latch,
                                     const ss::LatestMailbox<ss::ShowSnapshotLite>& show,
                                     const ss::LatestMailbox<ss::FeatureSnapshot>& features)
    : clock_(clock), real_(realSink), latch_(latch), show_(show), features_(features) {}

void LightingScheduler::setTickHandler(TickHandler h) {
    std::lock_guard<std::mutex> g(handlerMutex_);
    handler_ = std::move(h);
}

ss::Status LightingScheduler::arm() {
    if (!real_.connected())
        return ss::makeError(ss::ErrorCategory::DeviceLost, "no lighting transport", ss::Recoverability::OperatorAction);
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
    v.state = armed() ? LightingUiState::Armed : LightingUiState::Disarmed;
    v.transportConnected = real_.connected();
    v.blackout = blackout_.load(std::memory_order_relaxed);
    v.ticks = ticks_.load(); v.ticksHeldDisarmed = heldDisarmed_.load();
    v.framesSent = sentWhileArmed_.load();
    v.note = armed() ? "Armed" : "Disarmed, press Arm";
}

}  // namespace ss::app
