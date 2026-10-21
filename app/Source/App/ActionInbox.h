// Contract: thread-safe multi-producer action intake merged by the CONTROL thread (app-shell 4.3, APP-SH-02/04).
// Owner: shell-integration. UI, keyboard, MIDI, palette and remote sources call submit() from any thread.
// Normal actions: one bounded (256) SPSC ring per ActionSource, guarded on the producer side by a tiny mutex so
// several threads using the same source stay correct. Safety-critical actions never compete with them: they set
// the BlackoutLatch where relevant and use a separate priority ring that the control thread drains first.
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include "App/Mailboxes.h"
#include "ss/base/actions.hpp"
#include "ss/base/queue.hpp"

namespace ss::app {

struct SubmitResult {
    bool queued = false;        // false: dropped (queue full) or rejected up front
    bool latched = false;       // a blackout latch was set immediately
    std::string reason;         // sanitised, empty when queued
};

class ActionInbox {
public:
    static constexpr std::size_t kPerSourceCapacity = 256, kPriorityCapacity = 64;
    static constexpr std::size_t kSources = 6;     // ActionSource::Ui .. System

    ActionInbox() {
        for (auto& s : lanes_) s.q = std::make_unique<ss::SpscQueue<ss::Action>>(kPerSourceCapacity);
        prio_ = std::make_unique<ss::SpscQueue<ss::Action>>(kPriorityCapacity);
    }

    // Any thread except the audio thread.
    SubmitResult submit(const ss::Action& a) {
        SubmitResult r;
        const auto id = static_cast<ss::ActionId>(a.actionId);
        if (ss::isSafetyCritical(id)) {
            if (id == ss::ActionId::VisualBlackout || id == ss::ActionId::EmergencyBlackout) { latch_.setVisual(); r.latched = true; }
            if (id == ss::ActionId::LightingBlackout || id == ss::ActionId::EmergencyBlackout) { latch_.setLighting(); r.latched = true; }
            if (id == ss::ActionId::DisarmLighting) { latch_.requestDisarm(); r.latched = true; }
            std::lock_guard<std::mutex> g(prioMutex_);
            r.queued = prio_->push(a);
            if (!r.queued) { r.reason = "priority queue full"; priorityDropped_.fetch_add(1); }
            return r;
        }
        auto& lane = lanes_[laneOf(a.source)];
        std::lock_guard<std::mutex> g(lane.m);
        r.queued = lane.q->push(a);
        if (!r.queued) r.reason = "action queue full";
        return r;
    }

    // CONTROL thread only. Priority ring first, then sources in a fixed rotation so none starves.
    bool pop(ss::Action& out) {
        if (prio_->pop(out)) return true;
        for (std::size_t i = 0; i < kSources; ++i) {
            const std::size_t k = (rr_ + i) % kSources;
            if (lanes_[k].q->pop(out)) { rr_ = (k + 1) % kSources; return true; }
        }
        return false;
    }

    std::size_t pending() const {
        std::size_t n = prio_->size();
        for (auto& l : lanes_) n += l.q->size();
        return n;
    }
    std::uint64_t dropped() const {
        std::uint64_t n = priorityDropped_.load();
        for (auto& l : lanes_) n += l.q->dropped();
        return n;
    }
    BlackoutLatch& latch() { return latch_; }
    const BlackoutLatch& latch() const { return latch_; }

private:
    static std::size_t laneOf(ss::ActionSource s) {
        const auto i = static_cast<std::size_t>(s);
        return i < kSources ? i : kSources - 1;
    }
    struct Lane { std::mutex m; std::unique_ptr<ss::SpscQueue<ss::Action>> q; };
    std::array<Lane, kSources> lanes_;
    std::unique_ptr<ss::SpscQueue<ss::Action>> prio_;
    std::mutex prioMutex_;
    std::atomic<std::uint64_t> priorityDropped_{0};
    BlackoutLatch latch_;
    std::size_t rr_ = 0;     // CONTROL thread only
};

}  // namespace ss::app
