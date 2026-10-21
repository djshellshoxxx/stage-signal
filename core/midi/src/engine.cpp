#include "ss/midi/engine.hpp"
#include <algorithm>

namespace ss::midi {

namespace {
bool isCritical(const MappingEntry& e) { return isSafetyCritical(static_cast<ActionId>(e.map.actionId)); }
bool deviceActive(DeviceMatch d) { return d == DeviceMatch::Identity || d == DeviceMatch::NameConfirmed; }
double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }
}  // namespace

MidiEngine::MidiEngine() { matches_.reserve(32); }

void MidiEngine::setSysexEnabled(bool on) {
    sysex_ = on;
    out_.setSysexEnabled(on);
    for (auto& p : in_) p.parser.setSysexEnabled(on);
}

MidiEngine::InPort* MidiEngine::findIn(std::uint32_t id) {
    for (auto& p : in_) if (p.info.portId == id) return &p;
    return nullptr;
}
const MidiEngine::InPort* MidiEngine::findIn(std::uint32_t id) const {
    for (const auto& p : in_) if (p.info.portId == id) return &p;
    return nullptr;
}
const MidiEngine::OutPort* MidiEngine::findOut(std::uint32_t id) const {
    for (const auto& p : outs_) if (p.id == id) return &p;
    return nullptr;
}
const PortInfo* MidiEngine::inputPort(std::uint32_t portId) const {
    const InPort* p = findIn(portId);
    return p ? &p->info : nullptr;
}

// ---- ports -----------------------------------------------------------------------------------------------------

Generation MidiEngine::openInputPort(std::uint32_t portId, const std::string& identity, const std::string& name, Ns now) {
    InPort* p = findIn(portId);
    if (!p) {
        if (in_.size() >= kMaxPorts) return 0;
        in_.push_back(InPort{PortInfo{}, Parser(sysex_), RateLimiter(), InputCounters{}});
        p = &in_.back();
        p->info.portId = portId;
    }
    p->info.identity = identity;
    p->info.name = name;
    p->info.generation = ++genCounter_;
    p->info.available = true;
    p->info.everOpened = true;
    p->info.lostAt = 0;
    p->parser.reset();
    p->parser.setSysexEnabled(sysex_);
    // Reconnect clears the old generation: nothing queued before the loss may fire now.
    queue_.purgePort(portId);
    latch_.purgePortUnsafe(portId);
    for (auto& e : table_.entriesMutable()) { e.armed = true; e.hasTrigger = false; }
    (void)now;
    return p->info.generation;
}

void MidiEngine::inputPortLost(std::uint32_t portId, Ns now) {
    InPort* p = findIn(portId);
    if (!p || !p->info.available) return;
    p->info.available = false;
    p->info.lostAt = now;
    p->parser.reset();
    queue_.purgePort(portId);
    latch_.purgePortUnsafe(portId);
}

void MidiEngine::updateInputDevices(const std::vector<PortInfo>& present, Ns now) {
    for (auto& p : in_) {
        if (!p.info.available) continue;
        const bool still = std::any_of(present.begin(), present.end(),
                                       [&](const PortInfo& q) { return q.portId == p.info.portId; });
        if (!still) inputPortLost(p.info.portId, now);
    }
    for (const auto& q : present) {
        const InPort* p = findIn(q.portId);
        if (!p || !p->info.available) openInputPort(q.portId, q.identity, q.name, now);
    }
}

Generation MidiEngine::openOutputPort(std::uint32_t portId, const std::string& identity, const std::string& name, Ns now) {
    const Generation g = out_.openPort(portId, now);
    if (g == 0) return 0;
    for (auto& o : outs_) if (o.id == portId) { o.identity = identity; o.name = name; return g; }
    OutPort o; o.id = portId; o.identity = identity; o.name = name;
    outs_.push_back(std::move(o));
    return g;
}

void MidiEngine::outputPortLost(std::uint32_t portId, Ns now) {
    (void)now;
    out_.portLost(portId);
}

void MidiEngine::linkPorts(std::uint32_t inPortId, std::uint32_t outPortId) {
    for (auto& o : outs_) {
        if (o.id != outPortId) continue;
        if (std::find(o.linkedIn.begin(), o.linkedIn.end(), inPortId) == o.linkedIn.end()) o.linkedIn.push_back(inPortId);
        return;
    }
    OutPort o; o.id = outPortId; o.linkedIn.push_back(inPortId);
    outs_.push_back(std::move(o));
}

std::vector<std::uint32_t> MidiEngine::linkedInputs(std::uint32_t outPortId) const {
    std::vector<std::uint32_t> r;
    const OutPort* o = findOut(outPortId);
    if (!o) return r;
    r = o->linkedIn;
    if (!o->identity.empty())
        for (const auto& p : in_)
            if (p.info.identity == o->identity && std::find(r.begin(), r.end(), p.info.portId) == r.end())
                r.push_back(p.info.portId);
    return r;
}

std::vector<std::uint32_t> MidiEngine::linkedOutputs(std::uint32_t inPortId) const {
    std::vector<std::uint32_t> r;
    const InPort* ip = findIn(inPortId);
    for (const auto& o : outs_) {
        const bool byLink = std::find(o.linkedIn.begin(), o.linkedIn.end(), inPortId) != o.linkedIn.end();
        const bool byIdentity = ip && !o.identity.empty() && o.identity == ip->info.identity;
        if (byLink || byIdentity) r.push_back(o.id);
    }
    return r;
}

bool MidiEngine::isFeedbackRisk(std::uint32_t outPortId) const { return !linkedInputs(outPortId).empty(); }

// ---- input -----------------------------------------------------------------------------------------------------

void MidiEngine::onBytes(std::uint32_t portId, Generation gen, const std::uint8_t* bytes, std::size_t n, Ns now) {
    InPort* p = findIn(portId);
    if (!p) return;
    if (!p->info.available || gen != p->info.generation) { ++p->c.staleGeneration; return; }
    std::vector<Message> msgs;
    p->parser.feed(bytes, n, now, msgs);
    for (const auto& m : msgs) onMessage(portId, gen, m, now);
}

void MidiEngine::process(MappingEntry& e, InPort& ip, const Message& m, Ns now) {
    const ActionId aid = static_cast<ActionId>(e.map.actionId);
    const bool crit = isSafetyCritical(aid);
    Action a;
    a.actionId = e.map.actionId;
    a.source = ActionSource::Midi;           // origin tag: an action born here may not re-enter via MIDI output
    a.target = kAllOutputs;
    a.correlationId = ++corr_;
    const Generation gen = ip.info.generation;

    if (classifyAction(aid) == ActionClass::Continuous) {
        double v = 0.0;
        if (e.map.encoder != model::EncoderMode::Absolute) {
            if (m.kind != MsgKind::ControlChange) return;
            const int d = relativeDelta(e.map.encoder, m.data2);
            if (d == 0) return;
            e.value = clamp01(e.value + d * e.opt.relativeStep);
            v = e.value;
        } else {
            switch (m.kind) {
                case MsgKind::NoteOn: case MsgKind::ControlChange: v = normaliseAbsolute(e.map, m.data2); break;
                case MsgKind::NoteOff: v = normaliseAbsolute(e.map, 0); break;       // release returns to the low end
                case MsgKind::ProgramChange: v = clamp01(e.opt.triggerValue); break;
                default: return;
            }
        }
        if (aid == ActionId::SetBpm) v = kMinBpm + v * (kMaxBpm - kMinBpm);        // SetBpm carries BPM, not 0..1
        a.value = v;
        Pending p; p.action = a; p.mappingId = e.map.id; p.portId = ip.info.portId; p.generation = gen; p.at = now;
        queue_.pushContinuous(p);
        ++ip.c.actionsProduced;
        return;
    }

    bool fire = false;
    switch (m.kind) {
        case MsgKind::NoteOn: fire = m.data2 >= e.map.valueMin && m.data2 <= e.map.valueMax; break;
        case MsgKind::ProgramChange: fire = true; break;
        case MsgKind::ControlChange:
            if (normaliseAbsolute(e.map, m.data2) >= 0.5) { if (e.armed) { fire = true; e.armed = false; } }
            else e.armed = true;
            break;
        default: break;                                                              // note-off does not trigger
    }
    if (!fire) return;
    if (isSceneTrigger(aid) && !crit) {
        if (e.hasTrigger && now - e.lastTrigger < kSceneDebounceNs) { ++ip.c.debounced; return; }
        e.hasTrigger = true; e.lastTrigger = now;
    }
    a.value = e.opt.triggerValue;
    ++ip.c.actionsProduced;
    if (crit) { latch_.set(a, ip.info.portId, gen); return; }
    Pending p; p.action = a; p.mappingId = e.map.id; p.portId = ip.info.portId; p.generation = gen; p.at = now;
    queue_.pushDiscrete(p);
}

void MidiEngine::onMessage(std::uint32_t portId, Generation gen, const Message& raw, Ns now) {
    // Normalise defensively: a NoteOn with velocity 0 is a NoteOff even when the caller bypassed the Parser.
    Message tmp;
    const Message* mp = &raw;
    if (raw.kind == MsgKind::NoteOn && raw.data2 == 0) { tmp = raw; tmp.kind = MsgKind::NoteOff; tmp.fromZeroVelocity = true; mp = &tmp; }
    const Message& m = *mp;
    InPort* ip = findIn(portId);
    if (!ip) return;
    if (!ip->info.available || gen != ip->info.generation) { ++ip->c.staleGeneration; return; }
    ++ip->c.rx;
    switch (m.kind) {                                   // realtime/sysex input is counted, never rate-limited or mapped
        case MsgKind::Clock: ++ip->c.clockRx; return;
        case MsgKind::Start: case MsgKind::Continue: case MsgKind::Stop: ++ip->c.transportRx; return;
        case MsgKind::SysEx: ++ip->c.sysexRx; return;
        default: break;
    }
    if (learn_.active() && learn_.offer(ip->info, m)) { ++ip->c.learnConsumed; return; }

    matches_.clear();
    bool critical = false, anyTrigger = false;
    for (auto& e : table_.entriesMutable()) {
        if (!matchesSignature(e.map, m)) continue;
        const DeviceMatch dm = matchDevice(e, ip->info);
        if (dm == DeviceMatch::NameUnconfirmed) { ++ip->c.unconfirmedSkipped; continue; }
        if (!deviceActive(dm)) continue;
        matches_.push_back(&e);
        critical = critical || isCritical(e);
        if (classifyAction(static_cast<ActionId>(e.map.actionId)) == ActionClass::Trigger) anyTrigger = true;
    }

    if (!ip->limiter.tryAcquire(now)) {                 // 1000 msg/s/port ceiling; counter is the diagnostic
        ++ip->c.rateDropped;
        if (!critical) return;
        ++ip->c.rateCriticalBypass;                      // Stop/blackout class is never dropped
    }
    if (matches_.empty()) { ++ip->c.unmatched; return; }

    Signature sig;
    signatureOf(portId, m, sig);
    if (!critical && guard_.isEcho(linkedOutputs(portId), m, now)) { ++ip->c.echoDropped; return; }

    bool suspended = guard_.inputSuspended(sig);
    const bool pressLike = m.kind == MsgKind::NoteOn || m.kind == MsgKind::ProgramChange || m.kind == MsgKind::ControlChange;
    if (anyTrigger && pressLike && !suspended) {
        // identical-event storm detection on triggers (continuous knobs and relative encoders are exempt by construction)
        if (guard_.noteTrigger(sig, m.data2, now)) suspended = true;
    }
    for (MappingEntry* e : matches_) {
        if (suspended && !isCritical(*e)) { ++ip->c.suspendedDropped; continue; }
        process(*e, *ip, m, now);
    }
}

void MidiEngine::poll(Ns now, std::vector<Action>& out) {
    (void)now;
    latch_.drain(out);
    scratch_.clear();
    queue_.drain(scratch_);
    for (const auto& p : scratch_) {
        const InPort* ip = findIn(p.portId);
        if (!ip || !ip->info.available || ip->info.generation != p.generation) continue;   // stale generation
        out.push_back(p.action);
    }
}

// ---- status ----------------------------------------------------------------------------------------------------

MappingStatus MidiEngine::mappingStatus(std::uint32_t mappingId) const {
    MappingStatus st;
    const MappingEntry* e = table_.find(mappingId);
    if (!e) return st;
    for (const auto& ip : in_) {
        const DeviceMatch dm = matchDevice(*e, ip.info);
        if (dm == DeviceMatch::None) continue;
        if (dm == DeviceMatch::NameUnconfirmed) {
            if (st.availability == MappingAvailability::NoMatchingDevice) st.availability = MappingAvailability::NeedsNameConfirmation;
            continue;
        }
        if (ip.info.available) { st.availability = MappingAvailability::Available; st.unavailableSince = 0; return st; }
        st.availability = MappingAvailability::DeviceUnavailable;
        st.unavailableSince = ip.info.lostAt;
    }
    return st;
}

std::vector<std::pair<std::uint32_t, MappingStatus>> MidiEngine::allMappingStatus() const {
    std::vector<std::pair<std::uint32_t, MappingStatus>> v;
    for (const auto& e : table_.entries()) v.emplace_back(e.map.id, mappingStatus(e.map.id));
    return v;
}

const InputCounters* MidiEngine::counters(std::uint32_t portId) const {
    const InPort* p = findIn(portId);
    return p ? &p->c : nullptr;
}

InputCounters MidiEngine::totals() const {
    InputCounters t;
    for (const auto& p : in_) {
        const InputCounters& c = p.c;
        t.rx += c.rx; t.clockRx += c.clockRx; t.transportRx += c.transportRx; t.sysexRx += c.sysexRx;
        t.rateDropped += c.rateDropped; t.rateCriticalBypass += c.rateCriticalBypass;
        t.staleGeneration += c.staleGeneration; t.echoDropped += c.echoDropped;
        t.suspendedDropped += c.suspendedDropped; t.debounced += c.debounced; t.learnConsumed += c.learnConsumed;
        t.unmatched += c.unmatched; t.unconfirmedSkipped += c.unconfirmedSkipped; t.actionsProduced += c.actionsProduced;
    }
    return t;
}

// ---- output ----------------------------------------------------------------------------------------------------

Status MidiEngine::send(std::uint32_t outPortId, const Message& m, Ns deadline, ActionSource origin, Ns now) {
    if (!findOut(outPortId)) return makeError(ErrorCategory::DeviceLost, "unknown output port");
    if (guard_.outputSuspended(outPortId, m))
        return makeError(ErrorCategory::PermissionDenied, "output route suspended by feedback alarm");
    if (isFeedbackRisk(outPortId)) {
        if (!guard_.routeEnabled(outPortId))
            return makeError(ErrorCategory::PermissionDenied, "feedback-risk route needs explicit enable");
        if (origin == ActionSource::Midi) {
            for (std::uint32_t inId : linkedInputs(outPortId)) {
                const InPort* ip = findIn(inId);
                if (!ip) continue;
                for (const auto& e : table_.entries())
                    if (deviceActive(matchDevice(e, ip->info)) && matchesSignature(e.map, m))
                        return makeError(ErrorCategory::PermissionDenied, "action->MIDI->action recursion forbidden");
            }
        }
    }
    return out_.send(outPortId, m, deadline, origin, now);
}

void MidiEngine::dueOutput(Ns now, std::vector<OutPacket>& out) {
    out_.due(now, out, [this, now](const OutPacket& p) {
        if (guard_.outputSuspended(p.midi.portId, p.msg)) return false;
        guard_.recordOutput(p.midi.portId, p.msg, now);
        return true;
    });
}

}  // namespace ss::midi
