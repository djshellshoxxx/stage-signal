// MOD-12 bounded packet fan-out: each consumer owns a finite queue, producers never block (push-drop-with-counter).
// Overflow policy keeps the stream decodable: drop the oldest GOP (up to the next video key frame), never grow.
#pragma once
#include "broadcast/transport.h"
#include "core/queue.h"
namespace ss {

struct PacketQueueLimits { size_t maxPackets = 512; size_t maxBytes = 8u << 20; };
struct PacketQueueStats { uint64_t pushed = 0, dropped = 0, gopsDropped = 0, rejectedWhileClosed = 0; size_t depth = 0, bytes = 0, highWater = 0; };

class PacketQueue {
 public:
  explicit PacketQueue(PacketQueueLimits l = PacketQueueLimits()) : lim_(l) {}
  void setOpen(bool open);                       // closed => pushes are rejected and counted (no accumulation during outage)
  bool push(const MediaPacketPtr& p);            // never blocks; false if p was rejected/dropped
  MediaPacketPtr peek();                         // oldest, not removed
  void popFront();
  void clear();
  void reset();                                  // clear + zero stats + reopen (new session)
  void requireKeyframe(bool on);                 // discard Video packets until a key frame arrives (after reconnect)
  PacketQueueStats stats() const;
  bool needsKeyframe() const;
 private:
  void dropOldestGopLocked();
  mutable std::mutex m_; std::deque<MediaPacketPtr> q_; PacketQueueLimits lim_; size_t bytes_ = 0; bool open_ = true, needKey_ = false; PacketQueueStats st_;
};

// One producer, many consumers (e.g. network muxer, preview). Consumers are independent: a stalled one only loses its own packets.
class PacketFanout {
 public:
  size_t addConsumer(const std::string& name, PacketQueueLimits l = PacketQueueLimits());
  void publish(const MediaPacketPtr& p);
  PacketQueue& consumer(size_t i) { return *c_[i].q; }
  size_t count() const { return c_.size(); }
  const std::string& name(size_t i) const { return c_[i].name; }
 private:
  struct C { std::string name; std::unique_ptr<PacketQueue> q; };
  std::vector<C> c_;
};

} // namespace ss
