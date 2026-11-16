// Contract: LIGHT thread body (app-shell 4.1, step 11, APP-AD-07). Owner: engine-adapters-desktop.
// DISARMED by default and after every start. While disarmed the tick handler is given a NullPacketSink, so
// nothing can reach the network no matter what the handler does. arm() is an explicit operator action
// (AppCommandKind::ArmLighting); it is never called by restore, recovery or startup.
// The engine-adapters-desktop worker installs a handler that drives ss::lighting::LightingEngine::tick() with
// the sink from the context. This commit ships only the scheduling, the arm gate and the blackout latch read.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include "App/Mailboxes.h"
#include "App/Views/LightingView.h"
#include "Engine/IPacketSink.h"
#include "ss/base/types.hpp"

namespace ss::app {

struct LightingTickContext {
    ss::Ns now = 0;
    bool armed = false;
    bool blackout = false;                       // show snapshot OR the atomic latch
    ss::ShowSnapshotLite show;
    ss::FeatureSnapshot features;
    ss::lighting::IDatagramSink& sink;           // NullPacketSink while disarmed
};

class LightingScheduler {
public:
    using TickHandler = std::function<void(const LightingTickContext&)>;
    LightingScheduler(const ss::IClock& clock, IPacketSink& realSink, BlackoutLatch& latch,
                      const ss::LatestMailbox<ss::ShowSnapshotLite>& show,
                      const ss::LatestMailbox<ss::FeatureSnapshot>& features);

    void setTickHandler(TickHandler h);                 // any thread; applied at the next tick
    void setRateHz(int hz) { rateHz_ = hz < 1 ? 1 : (hz > 40 ? 40 : hz); }   // 25 default, 40 max (spec 6.2)
    int rateHz() const { return rateHz_; }

    ss::Status arm();                                   // refused (with a reason) unless the transport is connected
    void disarm();                                      // idempotent
    bool armed() const { return armed_.load(std::memory_order_acquire); }

    void tick();                                        // LIGHT thread (or tests)
    void fillView(LightingView& v) const;

private:
    const ss::IClock& clock_;
    IPacketSink& real_;
    NullPacketSink null_;
    BlackoutLatch& latch_;
    const ss::LatestMailbox<ss::ShowSnapshotLite>& show_;
    const ss::LatestMailbox<ss::FeatureSnapshot>& features_;
    std::atomic<bool> armed_{false};
    int rateHz_ = 25;
    std::mutex handlerMutex_;
    TickHandler handler_;
    std::atomic<std::uint64_t> ticks_{0}, heldDisarmed_{0}, sentWhileArmed_{0};
    std::atomic<bool> blackout_{false};
};

}  // namespace ss::app
