// StageSignal midi: output scheduler (REQ-M02, IF-08, spec section 14).
// Monotonic deadlines, MIDI clock at 24 PPQN from a configurable tempo source (opt-in), separately mapped
// Start/Stop/Continue, tracking of notes StageSignal itself owns, bounded SysEx (disabled by default).
// The device write happens in the app adapter on a worker thread: it calls due(now, ...) and sends each packet.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "ss/base/actions.hpp"
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/midi/feedback.hpp"
#include "ss/midi/limits.hpp"
#include "ss/midi/message.hpp"

namespace ss::midi {

class ITempoSource {
public:
    virtual ~ITempoSource() = default;
    virtual double bpm() const = 0;      // <= 0 / non-finite / outside 20..300 means "no usable tempo"
};
class FixedTempo final : public ITempoSource {
public:
    explicit FixedTempo(double bpm = 120.0) : bpm_(bpm) {}
    void set(double bpm) { bpm_ = bpm; }
    double bpm() const override { return bpm_; }
private:
    double bpm_;
};

struct OutPacket {
    ScheduledMidi midi;                 // bytes/size/deadline/portId/generation (IF-08); size 0 for SysEx
    Message msg;                        // decoded form (SysEx payload lives in msg.sysex)
    ActionSource origin = ActionSource::System;
    Ns lateNs = 0;                      // now - deadline at hand-over (>= 0)
};

struct ClockConfig {
    bool enabled = false;               // clock is opt-in
    std::uint32_t portId = 0;
};
// Which StageSignal action triggers each MIDI transport message. ActionId::None = not mapped (default).
struct TransportMap {
    ActionId start = ActionId::None, stop = ActionId::None, cont = ActionId::None;
};

struct OutputStats {
    std::uint64_t sent = 0, clockSent = 0, clockDiscarded = 0, staleDiscarded = 0, rateRejected = 0,
                  queueRejected = 0, portDiscarded = 0, tempoInvalid = 0, sysexSent = 0, gateDropped = 0;
    Ns maxLateNs = 0;
};

class OutputScheduler {
public:
    using Gate = std::function<bool(const OutPacket&)>;     // return false to drop at hand-over

    OutputScheduler();
    void setTempoSource(const ITempoSource* t) { tempo_ = t; }
    void setSysexEnabled(bool on) { sysexEnabled_ = on; }
    bool sysexEnabled() const { return sysexEnabled_; }

    // ---- ports ----
    Generation openPort(std::uint32_t portId, Ns now);       // new generation; queued old-generation items vanish
    void portLost(std::uint32_t portId);
    bool portOpen(std::uint32_t portId) const;
    Generation portGeneration(std::uint32_t portId) const;

    // ---- scheduling ----
    // Deadline <= now means "as soon as possible". Bounds are validated before anything is queued.
    Status send(std::uint32_t portId, const Message& m, Ns deadline, ActionSource origin, Ns now);
    Status sendSysex(std::uint32_t portId, const std::vector<std::uint8_t>& frame, Ns deadline, ActionSource origin, Ns now);

    // ---- clock and transport ----
    Status configureClock(const ClockConfig& c, Ns now);
    const ClockConfig& clockConfig() const { return clock_; }
    void setTransportMap(const TransportMap& m) { tmap_ = m; }
    Status sendTransport(MsgKind kind, Ns now);              // Start / Stop / Continue on the clock port
    // Feed show actions here; sends the mapped transport message (if any). Returns true if one was sent.
    bool onShowAction(ActionId a, Ns now);

    // ---- delivery ----
    void due(Ns now, std::vector<OutPacket>& out, const Gate& gate = {});
    Ns nextWake(Ns now) const;                               // earliest time the worker needs to call due() again

    // ---- owned-note cleanup (never an unsolicited All Notes Off) ----
    std::size_t ownedNoteCount(std::uint32_t portId) const;
    std::size_t releaseOwned(std::uint32_t portId, Ns now);  // note-offs only for notes we sent; drops queued note-ons
    std::size_t releaseAllOwned(Ns now);

    std::size_t pending() const { return heap_.size(); }
    const OutputStats& stats() const { return stats_; }

private:
    struct Item { OutPacket pkt; std::uint64_t seq = 0; int prio = 1; };
    struct PortState { std::uint32_t id = 0; Generation gen = 0; bool open = false; RateLimiter limiter; PortState() : limiter() {} };
    struct Owned { std::uint32_t port; std::uint8_t ch, note; };
    PortState* port(std::uint32_t id);
    const PortState* port(std::uint32_t id) const;
    Status enqueue(PortState& ps, const Message& m, Ns deadline, ActionSource origin, Ns now, bool exemptRate);
    void push(Item it);
    void generateClock(Ns now);
    void reanchor(Ns at);
    Ns clockDeadline(std::uint64_t k) const;
    static int priorityOf(MsgKind k);
    static bool mustDeliver(MsgKind k);
    void purgePort(std::uint32_t portId, bool onlyNoteOn);

    std::vector<PortState> ports_;
    std::vector<Item> heap_;
    std::vector<Owned> owned_;
    std::uint64_t seq_ = 0;
    const ITempoSource* tempo_ = nullptr;
    bool sysexEnabled_ = false;
    ClockConfig clock_;
    TransportMap tmap_;
    // clock grid: pulse k (1-based) after the anchor is due at anchor + round(k * 2.5e9 / bpm)
    Ns anchorNs_ = 0;
    std::uint64_t pulseK_ = 0;
    double anchorBpm_ = 0.0;
    Ns lastPulse_ = 0;
    bool tempoStalled_ = false;
    OutputStats stats_;
};

}  // namespace ss::midi
