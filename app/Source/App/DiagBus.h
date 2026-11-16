// Contract: diagnostics bus (app-shell 3 DiagBus, APP-SH-12/13). Owner: shell-integration.
// Any NON-realtime thread posts sanitised events; the IO thread drains them into the diag EventJournal and the
// log sink. The audio thread never posts (it uses AudioIngress counters). Bounded: when full, the oldest
// pending event is dropped and counted. Posting takes a short mutex; do not post from a per-sample path.
#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "ss/base/time.hpp"

namespace ss::app {

enum class DiagLevel : unsigned char { Info, Warn, Error };

struct DiagEvent {
    ss::Ns atNs = 0;
    DiagLevel level = DiagLevel::Info;
    std::string tag;        // "start.audio", "shutdown.stage2.timeout", "adapter.midi", ...
    std::string text;       // already sanitised by the caller (security::sanitizeForLog for anything external)
};

class DiagBus {
public:
    static constexpr std::size_t kPending = 512, kRecent = 64;
    explicit DiagBus(const ss::IClock& clock) : clock_(clock) {}

    void post(DiagLevel level, std::string tag, std::string text) {
        std::lock_guard<std::mutex> g(m_);
        if (pending_.size() >= kPending) { pending_.pop_front(); ++dropped_; }
        pending_.push_back(DiagEvent{clock_.nowMonotonic(), level, std::move(tag), std::move(text)});
    }
    // IO thread. Moves pending events out and remembers the last kRecent for the UI.
    std::size_t drain(std::vector<DiagEvent>& out) {
        std::lock_guard<std::mutex> g(m_);
        const std::size_t n = pending_.size();
        for (auto& e : pending_) {
            recent_.push_back(e);
            if (recent_.size() > kRecent) recent_.pop_front();
            out.push_back(std::move(e));
        }
        pending_.clear();
        return n;
    }
    std::vector<DiagEvent> recent() const {
        std::lock_guard<std::mutex> g(m_);
        return std::vector<DiagEvent>(recent_.begin(), recent_.end());
    }
    std::uint64_t dropped() const { std::lock_guard<std::mutex> g(m_); return dropped_; }

private:
    const ss::IClock& clock_;
    mutable std::mutex m_;
    std::deque<DiagEvent> pending_, recent_;
    std::uint64_t dropped_ = 0;
};

}  // namespace ss::app
