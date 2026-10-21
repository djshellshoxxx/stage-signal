// StageSignal midi: bounded input event queue (512) with the overflow policy of spec section 8, plus the independent
// priority latch that carries Stop/blackout-class actions around the ordinary queue (REQ-M04, RISK-09).
// Not thread-safe: the app owns one MIDI worker thread (or a lock) around the engine.
#pragma once
#include <cstdint>
#include <vector>
#include "ss/base/types.hpp"
#include "ss/midi/limits.hpp"

namespace ss::midi {

struct Pending {
    Action action;
    std::uint32_t mappingId = 0;
    std::uint32_t portId = 0;
    Generation generation = 0;
    bool continuous = false;
    Ns at = 0;
};

struct QueueCounters {
    std::uint64_t coalesced = 0, droppedContinuous = 0, droppedDiscrete = 0, purgedStale = 0;
    std::uint64_t lost() const { return droppedContinuous + droppedDiscrete; }
};

class PendingQueue {
public:
    explicit PendingQueue(std::size_t capacity = kEventQueueCapacity);
    // Continuous controls coalesce per mapping to the latest value (position of the older entry is kept).
    void pushContinuous(const Pending& p);
    // Discrete triggers keep order. When full, the oldest continuous entry is dropped first, then the oldest trigger.
    void pushDiscrete(const Pending& p);
    std::size_t purgePort(std::uint32_t portId);       // drop everything from a port (loss / new generation)
    void drain(std::vector<Pending>& out);             // FIFO; empties the queue
    std::size_t size() const { return items_.size(); }
    std::size_t capacity() const { return cap_; }
    const QueueCounters& counters() const { return c_; }
private:
    bool makeRoom(bool incomingContinuous);
    std::size_t cap_;
    std::vector<Pending> items_;
    QueueCounters c_;
};

// Safety-critical actions (blackout, stop, disarm...). One slot per action id: repeated presses collapse, and a
// full ordinary queue can never displace them. Drained before the ordinary queue.
class PriorityLatch {
public:
    void set(const Action& a, std::uint32_t portId, Generation gen);
    // Reconnect: stale entries are dropped, except safe-direction actions (blackouts, stops, disarm) which are kept.
    std::size_t purgePortUnsafe(std::uint32_t portId);
    void drain(std::vector<Action>& out);
    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
private:
    struct Slot { Action action; std::uint32_t portId; Generation gen; };
    std::vector<Slot> items_;
};

}  // namespace ss::midi
