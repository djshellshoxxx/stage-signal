// LightingEngine (MOD-10): resolves fixture state each lighting tick and hands datagrams to a sink.
//   Disarmed (default) and DryRun route ONLY to the internal MockSink. Armed + Purpose::Live is the single path
//   that reaches the real sink. Preview/Setup/Benchmark purposes ALWAYS use the MockSink (REQ-L03).
// The engine owns no threads and no sockets. The caller (app lighting worker) calls tick() at the lighting rate;
// latchBlackout()/restoreBlackout() are the only members safe to call from another thread.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/lighting/addressing.hpp"
#include "ss/lighting/resolver.hpp"
#include "ss/lighting/sacn.hpp"
#include "ss/lighting/sink.hpp"
#include "ss/model/show_model.hpp"

namespace ss::lighting {

struct LightingConfig {
    LightingProtocol protocol = LightingProtocol::ArtNet;
    Ipv4 destination;                          // unicast node (Art-Net: directed broadcast address when broadcast == true)
    bool broadcast = false;                    // Art-Net only; explicit opt-in
    bool multicast = false;                    // sACN only; 239.255.hi.lo per universe
    std::uint8_t sacnPriority = kSacnDefaultPriority;
    int rateHz = kDefaultRateHz;               // 1..40
    sacn::Cid cid{};                           // sACN source CID: persist per installation; never all zero
    std::string sourceName = "StageSignal";
    bool artnetDiscovery = false;              // opt-in ArtPoll; the engine itself never sends it
};
// Maps the show document settings. Destination text is strictly parsed; errors are InvalidArgument.
Result<LightingConfig> configFromSettings(const model::LightingSettings& s, const sacn::Cid& cid, const std::string& sourceName);
Status validateConfig(const LightingConfig& c);
// Broadcast ArtPoll datagram, available only when artnetDiscovery was explicitly enabled. The app decides when
// (and through which sink) to send it; it is never part of the output stream.
Result<Datagram> makeArtPollDatagram(const LightingConfig& c);

enum class EngineState : std::uint8_t { Disarmed, Armed, DryRun };
enum class Purpose : std::uint8_t { Live, Preview, Setup, Benchmark };
enum class EventKind : std::uint8_t {
    Armed, Disarmed, DryRunStarted, ArmRefused, TransportFault, TransportReconnected, BlackoutLatched, BlackoutReleased,
    StaleInputsDiscarded, GracefulStop
};
struct EngineEvent { Ns at = 0; EventKind kind = EventKind::Armed; std::string detail; };

struct LightingInputs {
    Purpose purpose = Purpose::Live;
    Generation generation = 0;                 // transport generation the inputs were built for; 0 = unstamped
    ResolveInputs resolve;
};

struct TickResult {
    std::vector<UniverseFrame> frames;         // latest state of every patched universe (always filled)
    EngineState state = EngineState::Disarmed;
    bool due = false;                          // rate gate open (or bypassed by the priority latch)
    bool transmitted = false;
    bool toRealSink = false;
    std::uint32_t datagrams = 0;
    bool priorityBypass = false;               // blackout / strobe-lock edge that skipped the rate gate
    bool staleInputs = false;
    BlackoutKind blackout = BlackoutKind::None;
    bool strobeLockApplied = false;
};

struct EngineStats {
    std::uint64_t ticks = 0, realDatagrams = 0, mockDatagrams = 0, staleInputs = 0, faults = 0, priorityBypasses = 0,
                  droppedEvents = 0;
};

class LightingEngine {
public:
    explicit LightingEngine(IDatagramSink* realSink, Generation generation = 1);

    // Only while Disarmed or DryRun. Replaces patch/config; keeps the engine Disarmed.
    Status configure(const std::vector<FixtureProfile>& profiles, const std::vector<FixturePatch>& patches,
                     const LightingConfig& cfg, const PatchOptions& opt = {}, const std::vector<AudioMapping>& mappings = {});

    // ---- state machine ----
    Status arm();                              // explicit Arm: patch armable, config valid, sink present and connected
    Status startDryRun();                      // mock-only; allowed with conflicts so the operator can inspect output
    void disarm(Ns now);                       // Armed: safe frames + protocol termination, then Disarmed
    void stop(Ns now) { disarm(now); }         // graceful stop at shutdown (same sequence)
    EngineState state() const { return state_; }
    bool needsExplicitArm() const { return faulted_; }

    // ---- priority latch (thread-safe): wins by the next tick and bypasses the rate gate/ordinary queue ----
    void latchBlackout(bool emergency);
    void restoreBlackout();                    // explicit restore of both latches
    bool blackoutLatched() const { return latchLighting_.load() || latchEmergency_.load(); }

    // ---- transport events (called by the app) ----
    void reportTransportLost(Ns now, const std::string& detail);
    void transportReconnected(Ns now, Generation newGeneration);   // stays Disarmed until arm()
    Generation generation() const { return generation_; }

    const TickResult& tick(Ns now, const LightingInputs& in);

    // ---- observability ----
    const MockSink& mockSink() const { return mock_; }
    MockSink& mockSink() { return mock_; }
    const EngineStats& stats() const { return stats_; }
    const PatchReport& patchReport() const { return resolver_.report(); }
    const FixtureResolver& resolver() const { return resolver_; }
    std::vector<EngineEvent> drainEvents();
    std::uint64_t armToken() const { return armToken_; }

private:
    void event(Ns at, EventKind k, std::string detail);
    void fault(Ns now, const std::string& detail);
    // Builds and sends every frame to `sink`; returns false on the first send error.
    bool transmit(const std::vector<UniverseFrame>& frames, IDatagramSink& sink, bool priority, bool terminated,
                  bool preview, std::uint32_t& count);
    Result<Datagram> build(std::size_t uidx, const UniverseFrame& f, bool priority, bool terminated, bool preview);

    IDatagramSink* real_;
    MockSink mock_;
    Generation generation_;
    LightingConfig cfg_;
    FixtureResolver resolver_;
    bool configured_ = false, faulted_ = false;
    EngineState state_ = EngineState::Disarmed;
    std::uint64_t armToken_ = 0, nextToken_ = 1;
    std::atomic<bool> latchLighting_{false}, latchEmergency_{false};
    bool prevLatchSeen_ = false, staleSeen_ = false;
    Ns lastNow_ = 0;
    // rate gates, [0] = real sink route, [1] = mock route (previews never disturb live pacing)
    struct Gate { bool tx = false; Ns last = 0; bool blackout = false, strobe = false; };
    std::array<Gate, 2> gate_{};
    std::array<std::uint8_t, kMaxUniverses> artSeq_{}, sacnSeq_{};
    ResolveInputs lastGood_;
    TickResult result_;
    EngineStats stats_;
    std::vector<EngineEvent> events_;
};

}  // namespace ss::lighting
