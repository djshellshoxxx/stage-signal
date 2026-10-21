#include "ss/midi/feedback.hpp"
#include <algorithm>

namespace ss::midi {

RateLimiter::RateLimiter(std::uint32_t ceiling, Ns windowNs)
    : ring_(ceiling ? ceiling : 1, 0), window_(windowNs) {}

bool RateLimiter::tryAcquire(Ns now) {
    const std::size_t cap = ring_.size();
    if (count_ < cap) {
        ring_[(head_ + count_) % cap] = now;
        ++count_; ++admitted_;
        return true;
    }
    if (now - ring_[head_] >= window_) {              // oldest admission left the window
        ring_[head_] = now;
        head_ = (head_ + 1) % cap;
        ++admitted_;
        return true;
    }
    ++rejected_;
    return false;
}

bool signatureOf(std::uint32_t port, const Message& m, Signature& out) {
    if (!m.isChannelMessage()) return false;
    out.port = port; out.channel = m.channel; out.number = m.data1;
    switch (m.kind) {
        case MsgKind::NoteOn: case MsgKind::NoteOff: out.type = model::MidiMsgType::Note; return true;
        case MsgKind::ControlChange: out.type = model::MidiMsgType::ControlChange; return true;
        case MsgKind::ProgramChange: out.type = model::MidiMsgType::ProgramChange; return true;
        default: return false;
    }
}

void FeedbackGuard::setRouteEnabled(std::uint32_t outPort, bool on) {
    auto it = std::find(enabled_.begin(), enabled_.end(), outPort);
    if (on && it == enabled_.end()) enabled_.push_back(outPort);
    if (!on && it != enabled_.end()) enabled_.erase(it);
}
bool FeedbackGuard::routeEnabled(std::uint32_t outPort) const {
    return std::find(enabled_.begin(), enabled_.end(), outPort) != enabled_.end();
}

void FeedbackGuard::recordOutput(std::uint32_t outPort, const Message& m, Ns now) {
    Signature s;
    if (!signatureOf(outPort, m, s)) return;
    OutRec r{outPort, s.type, s.channel, s.number, now};
    if (history_.size() < kEchoHistory) history_.push_back(r);
    else { history_[histHead_] = r; histHead_ = (histHead_ + 1) % kEchoHistory; }
}

bool FeedbackGuard::isEcho(const std::vector<std::uint32_t>& outPorts, const Message& m, Ns now) const {
    Signature s;
    if (!signatureOf(0, m, s)) return false;
    for (const auto& r : history_) {
        if (now - r.at > kEchoWindowNs || now < r.at) continue;
        if (r.type != s.type || r.ch != s.channel || r.num != s.number) continue;
        if (std::find(outPorts.begin(), outPorts.end(), r.outPort) != outPorts.end()) return true;
    }
    return false;
}

bool FeedbackGuard::noteTrigger(const Signature& sig, std::uint8_t value, Ns now) {
    Track* t = nullptr;
    for (auto& k : tracks_) if (k.sig == sig && k.value == value) { t = &k; break; }
    if (!t) {
        if (tracks_.size() >= kMaxTrackedStormKeys) {            // evict the least recently used key
            auto victim = std::min_element(tracks_.begin(), tracks_.end(),
                                           [](const Track& a, const Track& b) { return a.last < b.last; });
            tracks_.erase(victim);
        }
        tracks_.push_back(Track{sig, value, {}, now});
        t = &tracks_.back();
    }
    t->last = now;
    t->times.erase(std::remove_if(t->times.begin(), t->times.end(),
                                  [&](Ns x) { return now - x >= kFeedbackWindowNs; }), t->times.end());
    t->times.push_back(now);
    if (static_cast<int>(t->times.size()) <= kFeedbackTriggerLimit) return false;
    for (const auto& a : alarms_) if (a.sig == sig) return false;   // already latched
    FeedbackAlarm a;
    a.sig = sig; a.count = static_cast<std::uint32_t>(t->times.size()); a.at = now;
    // Suspend any output route we recently sent the same signature on: that is the other half of the loop.
    for (const auto& r : history_) {
        if (now - r.at > kFeedbackWindowNs || now < r.at) continue;
        if (r.type != sig.type || r.ch != sig.channel || r.num != sig.number) continue;
        bool dup = false;
        for (const auto& o : outSusp_) if (o.outPort == r.outPort && o.type == r.type && o.ch == r.ch && o.num == r.num) dup = true;
        if (!dup) outSusp_.push_back({r.outPort, r.type, r.ch, r.num, sig});
        a.outputSuspended = true;
    }
    alarms_.push_back(a);
    return true;
}

bool FeedbackGuard::inputSuspended(const Signature& sig) const {
    for (const auto& a : alarms_) if (a.sig == sig) return true;
    return false;
}

bool FeedbackGuard::outputSuspended(std::uint32_t outPort, const Message& m) const {
    Signature s;
    if (!signatureOf(outPort, m, s)) return false;
    for (const auto& o : outSusp_)
        if (o.outPort == outPort && o.type == s.type && o.ch == s.channel && o.num == s.number) return true;
    return false;
}

bool FeedbackGuard::clearAlarm(const Signature& sig) {
    auto it = std::find_if(alarms_.begin(), alarms_.end(), [&](const FeedbackAlarm& a) { return a.sig == sig; });
    if (it == alarms_.end()) return false;
    alarms_.erase(it);
    outSusp_.erase(std::remove_if(outSusp_.begin(), outSusp_.end(), [&](const OutSusp& o) { return o.owner == sig; }),
                   outSusp_.end());
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.sig == sig; }),
                  tracks_.end());
    return true;
}

void FeedbackGuard::reset() {
    enabled_.clear(); history_.clear(); histHead_ = 0; tracks_.clear(); alarms_.clear(); outSusp_.clear();
}

}  // namespace ss::midi
