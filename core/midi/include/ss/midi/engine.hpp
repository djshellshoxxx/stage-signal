// StageSignal midi: MidiEngine = MOD-09 portable core. One owner thread (the app's MIDI worker) calls everything.
//   device callbacks -> onBytes/onMessage -> [generation, learn, rate, echo, mapping, debounce, coalesce]
//   -> priority latch + 512 queue -> poll() -> ss::Action items for the show engine (IF-02, source = Midi)
//   show events / UI -> send() -> feedback policy -> OutputScheduler -> dueOutput() -> device write.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/midi/feedback.hpp"
#include "ss/midi/learn.hpp"
#include "ss/midi/mapping.hpp"
#include "ss/midi/output.hpp"
#include "ss/midi/pending_queue.hpp"

namespace ss::midi {

struct InputCounters {
    std::uint64_t rx = 0, clockRx = 0, transportRx = 0, sysexRx = 0;
    std::uint64_t rateDropped = 0, rateCriticalBypass = 0, staleGeneration = 0;
    std::uint64_t echoDropped = 0, suspendedDropped = 0, debounced = 0, learnConsumed = 0;
    std::uint64_t unmatched = 0, unconfirmedSkipped = 0, actionsProduced = 0;
};

enum class MappingAvailability : std::uint8_t {
    Available, DeviceUnavailable /*seen, now lost*/, NeedsNameConfirmation, NoMatchingDevice /*never seen this session*/
};
struct MappingStatus {
    MappingAvailability availability = MappingAvailability::NoMatchingDevice;
    Ns unavailableSince = 0;           // mono time the loss was detected (DeviceUnavailable only)
    bool usable() const { return availability == MappingAvailability::Available; }
};

class MidiEngine {
public:
    MidiEngine();

    MappingTable& mappings() { return table_; }
    const MappingTable& mappings() const { return table_; }
    LearnSession& learn() { return learn_; }
    FeedbackGuard& feedback() { return guard_; }
    const FeedbackGuard& feedback() const { return guard_; }
    OutputScheduler& scheduler() { return out_; }   // clock/transport/owned-note configuration; use send() for messages
    void setSysexEnabled(bool on);                  // off by default; applies to input parsers and output

    // ---- ports (called by the app adapter on enumeration / open / loss) ----
    Generation openInputPort(std::uint32_t portId, const std::string& identity, const std::string& name, Ns now);
    void inputPortLost(std::uint32_t portId, Ns now);
    // Enumeration poll: ports missing from `present` are treated as lost, new ones as opened. Poll <= kDevicePollPeriodNs.
    void updateInputDevices(const std::vector<PortInfo>& present, Ns now);
    const PortInfo* inputPort(std::uint32_t portId) const;
    Generation openOutputPort(std::uint32_t portId, const std::string& identity, const std::string& name, Ns now);
    void outputPortLost(std::uint32_t portId, Ns now);
    void linkPorts(std::uint32_t inPortId, std::uint32_t outPortId);   // declare that an output feeds an input back
    bool isFeedbackRisk(std::uint32_t outPortId) const;
    void setFeedbackRouteEnabled(std::uint32_t outPortId, bool on) { guard_.setRouteEnabled(outPortId, on); }

    // ---- input ----
    void onBytes(std::uint32_t portId, Generation gen, const std::uint8_t* bytes, std::size_t n, Ns now);
    void onMessage(std::uint32_t portId, Generation gen, const Message& m, Ns now);
    void poll(Ns now, std::vector<Action>& out);       // priority latch first, then the queue in FIFO order
    MappingStatus mappingStatus(std::uint32_t mappingId) const;
    std::vector<std::pair<std::uint32_t, MappingStatus>> allMappingStatus() const;
    const InputCounters* counters(std::uint32_t portId) const;
    InputCounters totals() const;
    const QueueCounters& queueCounters() const { return queue_.counters(); }
    std::size_t queued() const { return queue_.size() + latch_.size(); }

    // ---- output ----
    Status send(std::uint32_t outPortId, const Message& m, Ns deadline, ActionSource origin, Ns now);
    void dueOutput(Ns now, std::vector<OutPacket>& out);

private:
    struct InPort { PortInfo info; Parser parser; RateLimiter limiter; InputCounters c; };
    struct OutPort { std::uint32_t id = 0; std::string identity, name; std::vector<std::uint32_t> linkedIn; };
    InPort* findIn(std::uint32_t id);
    const InPort* findIn(std::uint32_t id) const;
    const OutPort* findOut(std::uint32_t id) const;
    std::vector<std::uint32_t> linkedOutputs(std::uint32_t inPortId) const;
    std::vector<std::uint32_t> linkedInputs(std::uint32_t outPortId) const;
    void process(MappingEntry& e, InPort& ip, const Message& m, Ns now);

    MappingTable table_;
    LearnSession learn_;
    FeedbackGuard guard_;
    OutputScheduler out_;
    PendingQueue queue_;
    PriorityLatch latch_;
    std::vector<InPort> in_;
    std::vector<OutPort> outs_;
    std::vector<MappingEntry*> matches_;
    std::vector<Pending> scratch_;
    Generation genCounter_ = 0;
    std::uint64_t corr_ = 0;
    bool sysex_ = false;
};

}  // namespace ss::midi
