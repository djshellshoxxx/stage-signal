// Contract: cross-thread exchange primitives used by AppController and UiModel. Owner: shell-integration.
// All of these are JUCE-free. Which one to use:
//   LatestMailbox<T> (core/base)  small trivially-copyable state, seqlock, writer never waits
//   LatestShared<T>               larger immutable snapshots (publisher allocates one object per publish)
//   FrameExchange                 pooled RGBA frames, one writer (RENDER), any number of readers (UI)
//   BlackoutLatch                 atomics read directly by RENDER and LIGHT (app-shell 4.3)
//   Heartbeats                    one atomic timestamp per worker thread (APP-SH-13)
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "ss/base/queue.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"

namespace ss::app {

// ---- LatestShared -------------------------------------------------------------------------------
// Publisher builds an immutable T and hands it over; readers get a shared_ptr that stays valid as long as they
// hold it. Not for the audio thread (publishing allocates). Reads never block a publisher.
template <class T>
class LatestShared {
public:
    void publish(std::shared_ptr<const T> p) { std::atomic_store(&p_, std::move(p)); version_.fetch_add(1, std::memory_order_release); }
    std::shared_ptr<const T> get() const { return std::atomic_load(&p_); }
    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }
private:
    std::shared_ptr<const T> p_;
    std::atomic<std::uint64_t> version_{0};
};

// ---- FrameExchange ------------------------------------------------------------------------------
struct FrameInfo {
    OutputId output = 0;
    Generation generation = 0;
    std::uint64_t sequence = 0;
    Ns producedNs = 0;
    bool blackout = false;
};

struct SharedFrame {
    PixelBuffer pixels;
    FrameInfo info;
};

// Pool of N frames. The writer takes a frame nobody else holds, fills it, publishes it. A reader keeps the
// shared_ptr as long as it needs the pixels; an unreleased frame just means the writer skips (and counts) a frame.
class FrameExchange {
public:
    // Writer thread (RENDER). Rebuilds the pool; frames still held by readers stay valid until released.
    void resize(std::uint32_t w, std::uint32_t h, std::size_t poolSize = 4) {
        pool_.clear();
        for (std::size_t i = 0; i < poolSize; ++i) {
            auto f = std::make_shared<SharedFrame>();
            f->pixels = PixelBuffer(w, h);
            pool_.push_back(std::move(f));
        }
        w_ = w; h_ = h;
    }
    std::uint32_t width() const { return w_; }
    std::uint32_t height() const { return h_; }
    // Writer thread. nullptr when every frame is held (dropped counter increments).
    std::shared_ptr<SharedFrame> acquireForWrite() {
        for (auto& f : pool_) if (f.use_count() == 1) return f;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    void publish(const std::shared_ptr<SharedFrame>& f) { latest_.publish(f); }
    // Any thread.
    std::shared_ptr<const SharedFrame> latest() const { return latest_.get(); }
    std::uint64_t version() const { return latest_.version(); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
private:
    std::vector<std::shared_ptr<SharedFrame>> pool_;   // writer-thread only
    std::uint32_t w_ = 0, h_ = 0;
    LatestShared<SharedFrame> latest_;
    std::atomic<std::uint64_t> dropped_{0};
};

// ---- BlackoutLatch ------------------------------------------------------------------------------
// Set directly from submitAction() for safety-critical blackout actions (before the control thread sees them).
// RENDER and LIGHT OR this with the show snapshot, so a stalled CONTROL thread cannot delay blackout beyond one
// tick (REQ-D08, REQ-L04). Only the CONTROL thread clears it, after the show engine accepted RestoreBlackout.
struct BlackoutLatch {
    std::atomic<bool> visual{false}, lighting{false};
    std::atomic<bool> disarmLighting{false};     // DisarmLighting: the LIGHT thread consumes it at its next tick
    void setVisual() { visual.store(true, std::memory_order_release); }
    void setLighting() { lighting.store(true, std::memory_order_release); }
    void requestDisarm() { disarmLighting.store(true, std::memory_order_release); }
    bool consumeDisarm() { return disarmLighting.exchange(false, std::memory_order_acq_rel); }
    void clearAll() { visual.store(false, std::memory_order_release); lighting.store(false, std::memory_order_release); }
    bool anyVisual() const { return visual.load(std::memory_order_acquire); }
    bool anyLighting() const { return lighting.load(std::memory_order_acquire); }
};

// ---- Heartbeats ---------------------------------------------------------------------------------
enum class ThreadId : std::uint8_t { Control, Analysis, Render, Light, MidiOut, Io, Count };

inline const char* threadName(ThreadId t) {
    switch (t) {
        case ThreadId::Control: return "CONTROL";
        case ThreadId::Analysis: return "ANALYSIS";
        case ThreadId::Render: return "RENDER";
        case ThreadId::Light: return "LIGHT";
        case ThreadId::MidiOut: return "MIDI-OUT";
        case ThreadId::Io: return "IO";
        case ThreadId::Count: break;
    }
    return "?";
}

class Heartbeats {
public:
    static constexpr Ns kStaleNs = 2 * kNsPerSec;      // APP-SH-13
    void beat(ThreadId t, Ns now) {
        last_[idx(t)].store(now, std::memory_order_relaxed);
        count_[idx(t)].fetch_add(1, std::memory_order_relaxed);
    }
    void markRunning(ThreadId t, bool on) { running_[idx(t)].store(on, std::memory_order_relaxed); }
    bool running(ThreadId t) const { return running_[idx(t)].load(std::memory_order_relaxed); }
    Ns last(ThreadId t) const { return last_[idx(t)].load(std::memory_order_relaxed); }
    std::uint64_t beats(ThreadId t) const { return count_[idx(t)].load(std::memory_order_relaxed); }
    bool stale(ThreadId t, Ns now) const { return running(t) && now - last(t) > kStaleNs; }
private:
    static std::size_t idx(ThreadId t) { return static_cast<std::size_t>(t); }
    std::array<std::atomic<Ns>, static_cast<std::size_t>(ThreadId::Count)> last_{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ThreadId::Count)> count_{};
    std::array<std::atomic<bool>, static_cast<std::size_t>(ThreadId::Count)> running_{};
};

}  // namespace ss::app
