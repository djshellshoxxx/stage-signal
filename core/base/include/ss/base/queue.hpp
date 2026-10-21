// StageSignal base: bounded, preallocated queues (spec section 8). No allocation after construction.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace ss {

// Single-producer / single-consumer lock-free ring. push() never blocks; when full it drops the
// newest item and counts it, so a slow consumer can never stall a capture thread.
template <class T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity) : cap_(capacity + 1), buf_(capacity + 1) {}
    bool push(const T& v) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t n = next(h);
        if (n == tail_.load(std::memory_order_acquire)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
        buf_[h] = v;
        head_.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t];
        tail_.store(next(t), std::memory_order_release);
        return true;
    }
    std::size_t size() const {
        const std::size_t h = head_.load(std::memory_order_acquire), t = tail_.load(std::memory_order_acquire);
        return h >= t ? h - t : h + cap_ - t;
    }
    std::size_t capacity() const { return cap_ - 1; }
    double occupancy() const { return capacity() ? static_cast<double>(size()) / static_cast<double>(capacity()) : 0.0; }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
private:
    std::size_t next(std::size_t i) const { return i + 1 == cap_ ? 0 : i + 1; }
    const std::size_t cap_;
    std::vector<T> buf_;
    std::atomic<std::size_t> head_{0}, tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

// Latest-value mailbox (seqlock) for small trivially-copyable state: lighting universes, feature
// snapshots, camera frame handles. Writer never waits; reader retries on a torn read.
template <class T>
class LatestMailbox {
    static_assert(std::is_trivially_copyable<T>::value, "LatestMailbox requires trivially copyable T");
public:
    void publish(const T& v) {
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);   // odd = write in progress
        std::atomic_thread_fence(std::memory_order_release);
        data_ = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }
    // Returns false until the first publish.
    bool read(T& out) const {
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t a = seq_.load(std::memory_order_acquire);
            if (a == 0) return false;
            if (a & 1u) continue;
            std::atomic_thread_fence(std::memory_order_acquire);
            T tmp = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_acquire) == a) { out = tmp; return true; }
        }
        return false;
    }
    std::uint32_t version() const { return seq_.load(std::memory_order_acquire) / 2; }
private:
    std::atomic<std::uint32_t> seq_{0};
    T data_{};
};

}  // namespace ss
