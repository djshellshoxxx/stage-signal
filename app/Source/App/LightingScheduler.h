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

// Beat-look modulation for lighting (FEAT-003 + key colour FEAT-002), packed into one atomic word by the shell.
struct BeatLightCtx {
    bool on = false;            // operator opt-in ("Beat pulse"); off by default
    bool valid = false;         // beat grid anchored with a tempo
    float value = 0.f;          // 0..1
    std::int64_t cycle = 0;     // low 16 bits only
    bool keyValid = false;
    std::uint8_t r = 0, g = 0, b = 0;
    static std::uint64_t pack(const BeatLightCtx& c) {
        return std::uint64_t(static_cast<std::uint8_t>(c.value < 0.f ? 0.f : (c.value > 1.f ? 255.f : c.value * 255.f + 0.5f))) |
               (std::uint64_t(c.valid) << 8) | (std::uint64_t(c.on) << 9) | (std::uint64_t(c.keyValid) << 10) |
               (std::uint64_t(c.r) << 16) | (std::uint64_t(c.g) << 24) | (std::uint64_t(c.b) << 32) |
               (std::uint64_t(static_cast<std::uint16_t>(c.cycle)) << 40);
    }
    static BeatLightCtx unpack(std::uint64_t w) {
        BeatLightCtx c;
        c.value = float(w & 255u) / 255.f; c.valid = (w >> 8) & 1u; c.on = (w >> 9) & 1u; c.keyValid = (w >> 10) & 1u;
        c.r = std::uint8_t(w >> 16); c.g = std::uint8_t(w >> 24); c.b = std::uint8_t(w >> 32);
        c.cycle = std::int64_t((w >> 40) & 0xffffu);
        return c;
    }
};

struct LightingTickContext {
    ss::Ns now = 0;
    bool armed = false;
    bool blackout = false;                       // show snapshot OR the atomic latch
    ss::ShowSnapshotLite show;
    ss::FeatureSnapshot features;
    ss::lighting::IDatagramSink& sink;           // NullPacketSink while disarmed
    BeatLightCtx beat;
};

// Drives one LightingEngine from LightingTickContext. All members except post()/stats are LIGHT-thread only.
class LightingDriver {
public:
    using Refused = std::function<void(const std::string&)>;       // engine refused / lost the arm: scheduler disarms
    using EngineJob = std::function<void(ss::lighting::LightingEngine&)>;
    LightingDriver(ss::lighting::IDatagramSink& real, Refused refused);
    void post(EngineJob job);                                       // any thread; applied at the start of the next tick
    void onTick(const LightingTickContext& ctx);
    // Fixture ids the beat look drives (every patched fixture); LIGHT thread applies it at the next tick.
    void setBeatFixtures(std::vector<std::uint32_t> ids);
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
    std::vector<ss::lighting::FixtureLook> looks_;        // LIGHT thread only
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
    BeatLightCtx beatLook() const { return BeatLightCtx::unpack(beat_.load(std::memory_order_relaxed)); }
    void setBeatLook(const BeatLightCtx& c) { beat_.store(BeatLightCtx::pack(c), std::memory_order_relaxed); }
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
    std::atomic<std::uint64_t> beat_{0};
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
