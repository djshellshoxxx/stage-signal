#include "ss/midi/output.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ss::midi {

namespace {
// Min-heap on (deadline, priority desc, seq): std heap functions build a max-heap on "less", so invert.
struct Later {
    template <class I> bool operator()(const I& a, const I& b) const {
        if (a.pkt.midi.deadline != b.pkt.midi.deadline) return a.pkt.midi.deadline > b.pkt.midi.deadline;
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.seq > b.seq;
    }
};
bool validData(const Message& m) {
    if (m.kind == MsgKind::SysEx) return true;
    if (m.isRealtime()) return true;
    return m.channel < 16 && m.data1 < 128 && m.data2 < 128;
}
}  // namespace

OutputScheduler::OutputScheduler() { heap_.reserve(kOutputQueueCapacity); }

int OutputScheduler::priorityOf(MsgKind k) {
    switch (k) {
        case MsgKind::Stop: return 5;
        case MsgKind::Start: case MsgKind::Continue: return 4;
        case MsgKind::Clock: return 3;
        case MsgKind::NoteOff: return 2;
        default: return 1;
    }
}
bool OutputScheduler::mustDeliver(MsgKind k) {
    return k == MsgKind::NoteOff || k == MsgKind::Start || k == MsgKind::Stop || k == MsgKind::Continue ||
           k == MsgKind::Clock;
}

OutputScheduler::PortState* OutputScheduler::port(std::uint32_t id) {
    for (auto& p : ports_) if (p.id == id) return &p;
    return nullptr;
}
const OutputScheduler::PortState* OutputScheduler::port(std::uint32_t id) const {
    for (const auto& p : ports_) if (p.id == id) return &p;
    return nullptr;
}
bool OutputScheduler::portOpen(std::uint32_t id) const { const auto* p = port(id); return p && p->open; }
Generation OutputScheduler::portGeneration(std::uint32_t id) const { const auto* p = port(id); return p ? p->gen : 0; }

void OutputScheduler::purgePort(std::uint32_t portId, bool onlyNoteOn) {
    const std::size_t before = heap_.size();
    heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [&](const Item& it) {
        if (it.pkt.midi.portId != portId) return false;
        return !onlyNoteOn || it.pkt.msg.kind == MsgKind::NoteOn;
    }), heap_.end());
    std::make_heap(heap_.begin(), heap_.end(), Later{});
    if (!onlyNoteOn) stats_.portDiscarded += before - heap_.size();
}

Generation OutputScheduler::openPort(std::uint32_t portId, Ns now) {
    PortState* ps = port(portId);
    if (!ps) {
        if (ports_.size() >= kMaxPorts) return 0;
        ports_.emplace_back();
        ps = &ports_.back();
        ps->id = portId;
    }
    purgePort(portId, false);                                 // nothing from an old generation survives
    ps->gen += 1;
    ps->open = true;
    if (clock_.enabled && clock_.portId == portId) reanchor(now);
    // Notes we owned before the connection dropped are ours to release (and only those).
    for (auto it = owned_.begin(); it != owned_.end();) {
        if (it->port == portId) {
            enqueue(*ps, makeNoteOff(it->ch, it->note), now, ActionSource::System, now, true);
            it = owned_.erase(it);
        } else ++it;
    }
    return ps->gen;
}

void OutputScheduler::portLost(std::uint32_t portId) {
    PortState* ps = port(portId);
    if (!ps) return;
    ps->open = false;
    purgePort(portId, false);
}

void OutputScheduler::push(Item it) {
    it.seq = ++seq_;
    it.prio = priorityOf(it.pkt.msg.kind);
    heap_.push_back(std::move(it));
    std::push_heap(heap_.begin(), heap_.end(), Later{});
}

Status OutputScheduler::enqueue(PortState& ps, const Message& m, Ns deadline, ActionSource origin, Ns now, bool exemptRate) {
    if (!ps.open) return makeError(ErrorCategory::DeviceLost, "output port is not open", Recoverability::OperatorAction);
    if (!validData(m)) return makeError(ErrorCategory::InvalidArgument, "MIDI data out of range");
    const bool must = mustDeliver(m.kind);
    if (!exemptRate && !must && !ps.limiter.tryAcquire(now)) {
        ++stats_.rateRejected;
        return makeError(ErrorCategory::ResourceExhausted, "output rate ceiling reached", Recoverability::Retryable);
    }
    if (deadline < now) deadline = now;
    // Continuous values already due collapse to the latest one.
    if (m.kind == MsgKind::ControlChange && deadline <= now) {
        for (auto& it : heap_) {
            const Message& q = it.pkt.msg;
            if (it.pkt.midi.portId == ps.id && q.kind == MsgKind::ControlChange && q.channel == m.channel &&
                q.data1 == m.data1 && it.pkt.midi.deadline <= now) {
                it.pkt.msg.data2 = m.data2; it.pkt.midi.bytes[2] = m.data2 & 0x7F;
                return okStatus();
            }
        }
    }
    if (!must) {
        std::size_t ordinary = 0;
        for (const auto& it : heap_) if (!mustDeliver(it.pkt.msg.kind)) ++ordinary;
        if (ordinary >= kOutputQueueCapacity) {
            // drop the stalest already-due continuous value, otherwise refuse the new item
            auto victim = heap_.end();
            for (auto it = heap_.begin(); it != heap_.end(); ++it)
                if (it->pkt.msg.kind == MsgKind::ControlChange && it->pkt.midi.deadline <= now &&
                    (victim == heap_.end() || it->seq < victim->seq)) victim = it;
            if (victim == heap_.end()) {
                ++stats_.queueRejected;
                return makeError(ErrorCategory::ResourceExhausted, "output queue full", Recoverability::Retryable);
            }
            heap_.erase(victim);
            std::make_heap(heap_.begin(), heap_.end(), Later{});
            ++stats_.staleDiscarded;
        }
    }
    if (heap_.size() >= kOutputHardCapacity) {
        ++stats_.queueRejected;
        return makeError(ErrorCategory::ResourceExhausted, "output queue full", Recoverability::Retryable);
    }
    Item it;
    it.pkt.msg = m;
    it.pkt.origin = origin;
    it.pkt.midi.size = static_cast<std::uint8_t>(encode(m, it.pkt.midi.bytes));
    it.pkt.midi.deadline = deadline;
    it.pkt.midi.portId = ps.id;
    it.pkt.midi.generation = ps.gen;
    push(std::move(it));
    return okStatus();
}

Status OutputScheduler::send(std::uint32_t portId, const Message& m, Ns deadline, ActionSource origin, Ns now) {
    PortState* ps = port(portId);
    if (!ps) return makeError(ErrorCategory::DeviceLost, "unknown output port");
    if (m.kind == MsgKind::SysEx) return sendSysex(portId, m.sysex, deadline, origin, now);
    return enqueue(*ps, m, deadline, origin, now, false);
}

Status OutputScheduler::sendSysex(std::uint32_t portId, const std::vector<std::uint8_t>& frame, Ns deadline,
                                  ActionSource origin, Ns now) {
    PortState* ps = port(portId);
    if (!ps) return makeError(ErrorCategory::DeviceLost, "unknown output port");
    if (!sysexEnabled_) return makeError(ErrorCategory::PermissionDenied, "SysEx output is disabled");
    if (frame.size() > kMaxSysexBytes) return makeError(ErrorCategory::InvalidArgument, "SysEx exceeds 1024 bytes");
    if (frame.size() < 2 || frame.front() != 0xF0 || frame.back() != 0xF7)
        return makeError(ErrorCategory::InvalidArgument, "SysEx must be a complete F0..F7 frame");
    for (std::size_t i = 1; i + 1 < frame.size(); ++i)
        if (frame[i] & 0x80) return makeError(ErrorCategory::InvalidArgument, "SysEx payload byte above 0x7F");
    Message m; m.kind = MsgKind::SysEx; m.sysex = frame;
    return enqueue(*ps, m, deadline, origin, now, false);
}

// ---- clock -----------------------------------------------------------------------------------------------------

static bool tempoUsable(double bpm) { return std::isfinite(bpm) && bpm >= kMinBpm && bpm <= kMaxBpm; }

void OutputScheduler::reanchor(Ns at) {
    anchorNs_ = at; pulseK_ = 0; lastPulse_ = at;
    const double bpm = tempo_ ? tempo_->bpm() : 0.0;
    if (tempoUsable(bpm)) { anchorBpm_ = bpm; tempoStalled_ = false; }
    else { anchorBpm_ = 0.0; tempoStalled_ = true; ++stats_.tempoInvalid; }
}

Ns OutputScheduler::clockDeadline(std::uint64_t k) const {
    return anchorNs_ + static_cast<Ns>(std::llround(static_cast<double>(k) * (2.5e9 / anchorBpm_)));
}

Status OutputScheduler::configureClock(const ClockConfig& c, Ns now) {
    clock_ = c;
    if (c.enabled) reanchor(now);
    return okStatus();
}

void OutputScheduler::generateClock(Ns now) {
    if (!clock_.enabled) return;
    PortState* ps = port(clock_.portId);
    if (!ps || !ps->open) return;
    const double bpm = tempo_ ? tempo_->bpm() : 0.0;
    if (!tempoUsable(bpm)) {
        if (!tempoStalled_) { tempoStalled_ = true; ++stats_.tempoInvalid; }
        return;                                               // hold: no pulses without a usable tempo
    }
    if (tempoStalled_) { reanchor(now); return; }             // resume cleanly from now
    if (std::fabs(bpm - anchorBpm_) > 1e-9) { anchorNs_ = lastPulse_; pulseK_ = 0; anchorBpm_ = bpm; }
    for (;;) {
        const Ns d = clockDeadline(pulseK_ + 1);
        if (d > now) break;
        ++pulseK_;
        lastPulse_ = d;
        if (now - d > kClockStaleNs) { ++stats_.clockDiscarded; continue; }     // never burst late pulses
        Item it;
        it.pkt.msg = makeRealtime(MsgKind::Clock);
        it.pkt.origin = ActionSource::System;
        it.pkt.midi.bytes[0] = 0xF8; it.pkt.midi.size = 1;
        it.pkt.midi.deadline = d; it.pkt.midi.portId = ps->id; it.pkt.midi.generation = ps->gen;
        push(std::move(it));
    }
}

Status OutputScheduler::sendTransport(MsgKind kind, Ns now) {
    if (kind != MsgKind::Start && kind != MsgKind::Stop && kind != MsgKind::Continue)
        return makeError(ErrorCategory::InvalidArgument, "not a transport message");
    if (!clock_.enabled) return makeError(ErrorCategory::UnsupportedCapability, "MIDI clock output is not enabled");
    PortState* ps = port(clock_.portId);
    if (!ps || !ps->open) return makeError(ErrorCategory::DeviceLost, "clock port is not open");
    if (kind != MsgKind::Stop) { generateClock(now); reanchor(now); }   // Start/Continue restart the pulse grid
    return enqueue(*ps, makeRealtime(kind), now, ActionSource::System, now, true);
}

bool OutputScheduler::onShowAction(ActionId a, Ns now) {
    if (a == ActionId::None) return false;
    MsgKind k;
    if (a == tmap_.stop) k = MsgKind::Stop;
    else if (a == tmap_.start) k = MsgKind::Start;
    else if (a == tmap_.cont) k = MsgKind::Continue;
    else return false;
    return sendTransport(k, now).ok();
}

// ---- delivery --------------------------------------------------------------------------------------------------

void OutputScheduler::due(Ns now, std::vector<OutPacket>& out, const Gate& gate) {
    generateClock(now);
    while (!heap_.empty() && heap_.front().pkt.midi.deadline <= now) {
        std::pop_heap(heap_.begin(), heap_.end(), Later{});
        Item it = std::move(heap_.back());
        heap_.pop_back();
        OutPacket& p = it.pkt;
        const PortState* ps = port(p.midi.portId);
        if (!ps || !ps->open || ps->gen != p.midi.generation) { ++stats_.portDiscarded; continue; }
        p.lateNs = now - p.midi.deadline;
        const MsgKind k = p.msg.kind;
        if (k == MsgKind::Clock && p.lateNs > kClockStaleNs) { ++stats_.clockDiscarded; continue; }
        if ((k == MsgKind::NoteOn || k == MsgKind::ControlChange || k == MsgKind::ProgramChange) &&
            p.lateNs > kNoteOnStaleNs) { ++stats_.staleDiscarded; continue; }
        if (gate && !gate(p)) { ++stats_.gateDropped; continue; }
        if (k == MsgKind::NoteOn) {
            bool have = false;
            for (const auto& o : owned_) if (o.port == p.midi.portId && o.ch == p.msg.channel && o.note == p.msg.data1) have = true;
            if (!have) owned_.push_back({p.midi.portId, p.msg.channel, p.msg.data1});
        } else if (k == MsgKind::NoteOff) {
            owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const Owned& o) {
                return o.port == p.midi.portId && o.ch == p.msg.channel && o.note == p.msg.data1; }), owned_.end());
        }
        ++stats_.sent;
        if (k == MsgKind::Clock) ++stats_.clockSent;
        if (k == MsgKind::SysEx) ++stats_.sysexSent;
        stats_.maxLateNs = std::max(stats_.maxLateNs, p.lateNs);
        out.push_back(std::move(p));
    }
}

Ns OutputScheduler::nextWake(Ns /*now*/) const {
    Ns best = std::numeric_limits<Ns>::max();
    if (!heap_.empty()) best = heap_.front().pkt.midi.deadline;
    if (clock_.enabled && !tempoStalled_ && anchorBpm_ > 0.0 && portOpen(clock_.portId))
        best = std::min(best, clockDeadline(pulseK_ + 1));
    return best;
}

// ---- owned notes -----------------------------------------------------------------------------------------------

std::size_t OutputScheduler::ownedNoteCount(std::uint32_t portId) const {
    std::size_t n = 0;
    for (const auto& o : owned_) if (o.port == portId) ++n;
    return n;
}

std::size_t OutputScheduler::releaseOwned(std::uint32_t portId, Ns now) {
    PortState* ps = port(portId);
    if (!ps || !ps->open) return 0;
    purgePort(portId, true);                                   // pending note-ons would become stuck notes
    std::size_t n = 0;
    for (auto it = owned_.begin(); it != owned_.end();) {
        if (it->port == portId) {
            if (enqueue(*ps, makeNoteOff(it->ch, it->note), now, ActionSource::System, now, true).ok()) ++n;
            it = owned_.erase(it);
        } else ++it;
    }
    return n;
}

std::size_t OutputScheduler::releaseAllOwned(Ns now) {
    std::vector<std::uint32_t> ids;
    for (const auto& o : owned_) if (std::find(ids.begin(), ids.end(), o.port) == ids.end()) ids.push_back(o.port);
    std::size_t n = 0;
    for (auto id : ids) n += releaseOwned(id, now);
    return n;
}

}  // namespace ss::midi
