// Bounded queues (spec §8): never block producers; drop policy explicit.
#pragma once
#include <mutex>
#include <deque>
#include <atomic>
#include <cstdint>
namespace ss {
// Mutex-protected bounded FIFO for non-realtime consumers. push() never blocks on consumers:
// returns false (and counts a drop) when full. Realtime producers must use tryPush with small critical section.
template <class T> class BoundedQueue {
 public:
  explicit BoundedQueue(size_t cap) : cap_(cap) {}
  bool push(T v) { std::lock_guard<std::mutex> l(m_); if (q_.size() >= cap_) { ++drops_; return false; } q_.push_back(std::move(v)); return true; }
  // Drops the OLDEST element to make room (newest-wins policy for frames / continuous CC).
  bool pushDropOldest(T v) { std::lock_guard<std::mutex> l(m_); bool dropped=false; if (q_.size() >= cap_) { q_.pop_front(); ++drops_; dropped=true; } q_.push_back(std::move(v)); return !dropped; }
  bool pop(T& out) { std::lock_guard<std::mutex> l(m_); if (q_.empty()) return false; out = std::move(q_.front()); q_.pop_front(); return true; }
  size_t size() const { std::lock_guard<std::mutex> l(m_); return q_.size(); }
  size_t capacity() const { return cap_; }
  uint64_t drops() const { return drops_.load(); }
  void clear() { std::lock_guard<std::mutex> l(m_); q_.clear(); }
 private:
  mutable std::mutex m_; std::deque<T> q_; size_t cap_; std::atomic<uint64_t> drops_{0};
};
// Latest-value mailbox (lighting per universe, camera newest-frame, feature snapshot).
template <class T> class Mailbox {
 public:
  void put(T v) { std::lock_guard<std::mutex> l(m_); v_ = std::move(v); has_ = true; ++ver_; }
  bool get(T& out) const { std::lock_guard<std::mutex> l(m_); if (!has_) return false; out = v_; return true; }
  uint64_t version() const { std::lock_guard<std::mutex> l(m_); return ver_; }
 private:
  mutable std::mutex m_; T v_{}; bool has_ = false; uint64_t ver_ = 0;
};
}
