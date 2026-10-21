// Contract: LIGHT thread body (app-shell 4.1, step 11, APP-AD-07). Owner: engine-adapters-desktop.
// DISARMED by default and after every start. While disarmed the tick handler is given a NullPacketSink, so
// nothing can reach the network no matter what the handler does. arm() is an explicit operator action
// (AppCommandKind::ArmLighting); it is never called by restore, recovery or startup.
// The scheduler owns a LightingDriver that is installed as the default tick handler: it owns the core
// ss::lighting::LightingEngine, keeps it in step with the arm gate, feeds it the show snapshot and audio features
// and counts frames. setTickHandler() still replaces it (tests). Patch/config changes and cue/manual looks are
// handed over with driver().post(fn) and run on the LIGHT thread.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include "App/Mailboxes.h"
#include "App/Views/LightingView.h"
#include "Engine/IPacketSink.h"
#include "ss/lighting/engine.hpp"
#include <memory>
#include <string>
#include <vector>
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

// Drives one LightingEngine from LightingTickContext. All members except post()/stats are LIGHT-thread only.
class LightingDriver {
public:
    using Refused = std::function<void(const std::string&)>;       // engine refused / lost the arm: scheduler disarms
    using EngineJob = std::function<void(ss::lighting::LightingEngine&)>;
    LightingDriver(ss::lighting::IDatagramSink& real, Refused refused);
    void post(EngineJob job);                                       // any thread; applied at the start of the next tick
    void onTick(const LightingTickContext& ctx);
    ss::lighting::EngineState engineState() const { return engineState_.load(); }
    std::uint64_t realDatagrams() const { return realDatagrams_.load(); }
    std::uint64_t mockDatagrams() const { return mockDatagrams_.load(); }
    std::uint64_t faults() const { return faults_.load(); }
    ss::lighting::LightingEngine& engine() { return engine_; }      // LIGHT thread or tests with the loop stopped
private:
    // Only open while Armed (and for the engine's own safe-frame / termination sequence on disarm).
    struct GateSink final : ss::lighting::IDatagramSink {
        explicit GateSink(ss::lighting::IDatagramSink& r) : real(r) {}
        ss::Status send(const ss::lighting::Datagram& d) override;
        bool connected() const override { return open.load(std::memory_order_acquire) && real.connected(); }
        ss::lighting::IDatagramSink& real;
        std::atomic<bool> open{false};
    };
    GateSink gate_;
    ss::lighting::LightingEngine engine_;
    Refused refused_;
    std::mutex jobsMutex_;
    std::vector<EngineJob> jobs_;
    std::atomic<ss::lighting::EngineState> engineState_{ss::lighting::EngineState::Disarmed};
    std::atomic<std::uint64_t> realDatagrams_{0}, mockDatagrams_{0}, faults_{0};
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
    LightingDriver& driver() { return *driver_; }

private:
    void noteRefused(const std::string& why);
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
    mutable std::mutex noteMutex_;
    std::string faultNote_;
    std::unique_ptr<LightingDriver> driver_;
};

}  // namespace ss::app
