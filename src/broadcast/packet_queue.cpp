#include "broadcast/packet_queue.h"
namespace ss {

void PacketQueue::setOpen(bool o) { std::lock_guard<std::mutex> l(m_); open_ = o; }
void PacketQueue::requireKeyframe(bool on) { std::lock_guard<std::mutex> l(m_); needKey_ = on; }
bool PacketQueue::needsKeyframe() const { std::lock_guard<std::mutex> l(m_); return needKey_; }
void PacketQueue::clear() { std::lock_guard<std::mutex> l(m_); q_.clear(); bytes_ = 0; }
void PacketQueue::reset() { std::lock_guard<std::mutex> l(m_); q_.clear(); bytes_ = 0; st_ = PacketQueueStats(); open_ = true; needKey_ = false; }
void PacketQueue::popFront() { std::lock_guard<std::mutex> l(m_); if (q_.empty()) return; bytes_ -= q_.front()->data.size(); q_.pop_front(); }
MediaPacketPtr PacketQueue::peek() { std::lock_guard<std::mutex> l(m_); return q_.empty() ? MediaPacketPtr() : q_.front(); }
PacketQueueStats PacketQueue::stats() const { std::lock_guard<std::mutex> l(m_); PacketQueueStats s = st_; s.depth = q_.size(); s.bytes = bytes_; return s; }

void PacketQueue::dropOldestGopLocked() {
  // remove packets up to (not including) the next video key frame after the first one; headers are never dropped
  size_t i = 0; bool seenKey = false;
  for (; i < q_.size(); ++i) {
    const MediaPacket& p = *q_[i];
    if (p.kind == PacketKind::Video && p.key) { if (seenKey || i > 0) break; seenKey = true; }
  }
  if (i == 0 || i >= q_.size()) i = 1;           // no later key frame in the queue: drop just the oldest packet
  size_t removed = 0;
  for (size_t k = 0; k < i && !q_.empty();) {
    if (q_.front()->kind == PacketKind::Headers) { q_.push_back(q_.front()); q_.pop_front(); ++k; continue; }   // rotate headers behind
    bytes_ -= q_.front()->data.size(); q_.pop_front(); ++removed; ++st_.dropped; ++k;
  }
  if (removed) ++st_.gopsDropped;
}

bool PacketQueue::push(const MediaPacketPtr& p) {
  std::lock_guard<std::mutex> l(m_);
  if (!open_) { ++st_.rejectedWhileClosed; return false; }
  if (needKey_ && p->kind == PacketKind::Video) { if (!p->key) { ++st_.dropped; return false; } needKey_ = false; }
  if (p->data.size() > lim_.maxBytes) { ++st_.dropped; return false; }
  int guard = 0;
  while ((q_.size() >= lim_.maxPackets || bytes_ + p->data.size() > lim_.maxBytes) && !q_.empty() && guard++ < 64) dropOldestGopLocked();
  if (q_.size() >= lim_.maxPackets) { ++st_.dropped; return false; }
  q_.push_back(p); bytes_ += p->data.size(); ++st_.pushed;
  if (q_.size() > st_.highWater) st_.highWater = q_.size();
  return true;
}

size_t PacketFanout::addConsumer(const std::string& n, PacketQueueLimits l) { C c; c.name = n; c.q.reset(new PacketQueue(l)); c_.push_back(std::move(c)); return c_.size() - 1; }
void PacketFanout::publish(const MediaPacketPtr& p) { for (auto& c : c_) c.q->push(p); }

} // namespace ss
