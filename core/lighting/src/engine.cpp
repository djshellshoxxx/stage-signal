#include "ss/lighting/engine.hpp"

#include "ss/lighting/artnet.hpp"

namespace ss::lighting {

namespace {
Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, why); }

bool cidZero(const sacn::Cid& c) { for (auto b : c) if (b) return false; return true; }

// Settings that must hold for any configuration (including dry-run/preview).
Status validateCommon(const LightingConfig& c) {
    if (c.rateHz < 1 || c.rateHz > kMaxRateHz) return bad("lighting rate must be 1..40 Hz");
    if (c.protocol == LightingProtocol::ArtNet && c.multicast) return bad("Art-Net has no multicast mode");
    if (c.protocol == LightingProtocol::Sacn && c.broadcast) return bad("sACN uses unicast or multicast, not broadcast");
    if (c.broadcast && c.multicast) return bad("choose broadcast or multicast, not both");
    if (c.protocol == LightingProtocol::Sacn) {
        if (c.sacnPriority > 200) return bad("sACN priority must be 0..200");
        if (cidZero(c.cid)) return bad("sACN source CID must be set");
    }
    return okStatus();
}
Endpoint artnetBroadcast(const LightingConfig& c) {
    Endpoint e; e.port = kArtNetPort;
    e.ip = c.destination.isZero() ? Ipv4{{{255, 255, 255, 255}}} : c.destination;
    return e;
}
}  // namespace

Status validateConfig(const LightingConfig& c) {            // strict: what Arm requires
    Status s = validateCommon(c);
    if (!s.ok()) return s;
    const bool haveDest = !c.destination.isZero();
    if (c.protocol == LightingProtocol::ArtNet) {
        if (!haveDest && !c.broadcast) return bad("Art-Net needs a unicast destination or explicit broadcast");
    } else {
        if (!haveDest && !c.multicast) return bad("sACN needs a unicast destination or explicit multicast");
    }
    return okStatus();
}

Result<LightingConfig> configFromSettings(const model::LightingSettings& s, const sacn::Cid& cid, const std::string& sourceName) {
    LightingConfig c;
    c.protocol = s.protocol;
    c.broadcast = s.broadcast; c.multicast = s.multicast;
    c.sacnPriority = s.sacnPriority > 200 ? std::uint8_t{255} : static_cast<std::uint8_t>(s.sacnPriority);   // 255 fails validation
    c.rateHz = s.rateHz;
    c.cid = cid; c.sourceName = sourceName;
    if (!s.destination.empty()) {
        auto ip = parseIpv4(s.destination);
        if (!ip.ok()) return ip.error();
        c.destination = ip.value();
    }
    Status v = validateCommon(c);
    if (!v.ok()) return v.error();
    return c;
}

Result<Datagram> makeArtPollDatagram(const LightingConfig& c) {
    if (c.protocol != LightingProtocol::ArtNet) return bad("ArtPoll applies to Art-Net only");
    if (!c.artnetDiscovery) return makeError(ErrorCategory::PermissionDenied, "Art-Net discovery is not enabled");
    Datagram d; d.dst = artnetBroadcast(c); d.bytes = artnet::buildPoll(artnet::PollParams{});
    return d;
}

LightingEngine::LightingEngine(IDatagramSink* realSink, Generation generation) : real_(realSink), generation_(generation) {}

void LightingEngine::event(Ns at, EventKind k, std::string detail) {
    if (events_.size() >= kMaxEvents) { events_.erase(events_.begin()); ++stats_.droppedEvents; }
    events_.push_back(EngineEvent{at, k, std::move(detail)});
}
std::vector<EngineEvent> LightingEngine::drainEvents() { std::vector<EngineEvent> out; out.swap(events_); return out; }

Status LightingEngine::configure(const std::vector<FixtureProfile>& profiles, const std::vector<FixturePatch>& patches,
                                 const LightingConfig& cfg, const PatchOptions& opt, const std::vector<AudioMapping>& mappings) {
    if (state_ == EngineState::Armed) return bad("disarm before changing the lighting configuration");
    Status v = validateCommon(cfg);
    if (!v.ok()) return v;
    Status s = resolver_.configure(profiles, patches, cfg.protocol, opt, mappings);
    if (!s.ok()) { configured_ = false; return s; }
    cfg_ = cfg;
    configured_ = true;
    gate_ = {};
    artSeq_ = {}; sacnSeq_ = {};
    return okStatus();
}

Status LightingEngine::arm() {
    if (!configured_) return bad("lighting is not configured");
    if (state_ == EngineState::Armed) return okStatus();
    if (!resolver_.armable()) {
        std::string why = "patch has unresolved issues";
        for (const auto& i : resolver_.report().issues) if (i.blocksArm) { why = i.message; break; }
        event(lastNow_, EventKind::ArmRefused, why);
        return makeError(ErrorCategory::InvalidArgument, "cannot arm: " + why);
    }
    Status v = validateConfig(cfg_);
    if (!v.ok()) { event(lastNow_, EventKind::ArmRefused, v.error().detail); return v; }
    if (!real_) { event(lastNow_, EventKind::ArmRefused, "no transport"); return makeError(ErrorCategory::UnsupportedCapability, "no lighting transport available"); }
    if (!real_->connected()) {
        event(lastNow_, EventKind::ArmRefused, "transport not connected");
        return makeError(ErrorCategory::DeviceLost, "lighting transport is not connected");
    }
    state_ = EngineState::Armed;
    faulted_ = false;
    armToken_ = nextToken_++;
    artSeq_ = {}; sacnSeq_ = {};
    gate_[0] = Gate{};
    event(lastNow_, EventKind::Armed, "armed");
    return okStatus();
}

Status LightingEngine::startDryRun() {
    if (!configured_) return bad("lighting is not configured");
    if (state_ == EngineState::Armed) disarm(lastNow_);
    state_ = EngineState::DryRun;
    gate_[1] = Gate{};
    event(lastNow_, EventKind::DryRunStarted, "dry run: mock sink only");
    return okStatus();
}

void LightingEngine::fault(Ns now, const std::string& detail) {
    state_ = EngineState::Disarmed;
    faulted_ = true;
    armToken_ = 0;
    ++stats_.faults;
    event(now, EventKind::TransportFault, detail);
}

void LightingEngine::reportTransportLost(Ns now, const std::string& detail) { fault(now, detail); }

void LightingEngine::transportReconnected(Ns now, Generation newGeneration) {
    generation_ = newGeneration;
    state_ = EngineState::Disarmed;       // reconnect NEVER re-arms (REQ-L05)
    faulted_ = true;
    armToken_ = 0;
    artSeq_ = {}; sacnSeq_ = {};
    gate_ = {};
    event(now, EventKind::TransportReconnected, "reconnected; remaining disarmed until explicit Arm");
}

void LightingEngine::latchBlackout(bool emergency) { (emergency ? latchEmergency_ : latchLighting_).store(true); }
void LightingEngine::restoreBlackout() { latchEmergency_.store(false); latchLighting_.store(false); }

Result<Datagram> LightingEngine::build(std::size_t uidx, const UniverseFrame& f, bool priority, bool terminated, bool preview) {
    Datagram d; d.generation = generation_; d.priority = priority;
    if (cfg_.protocol == LightingProtocol::ArtNet) {
        artnet::DmxParams p;
        p.address.wire = f.universe;
        artSeq_[uidx] = artnet::nextSequence(artSeq_[uidx]);
        p.sequence = artSeq_[uidx];
        auto pkt = artnet::buildDmx(p, f.slots.data(), kDmxSlots);
        if (!pkt.ok()) return pkt.error();
        d.bytes = std::move(pkt.value());
        d.dst = artnetBroadcast(cfg_);
        if (!cfg_.broadcast) d.dst.ip = cfg_.destination;
    } else {
        sacn::DataParams p;
        p.cid = cfg_.cid; p.sourceName = cfg_.sourceName; p.priority = cfg_.sacnPriority;
        p.universe.value = f.universe;
        p.sequence = sacnSeq_[uidx]++;
        p.preview = preview; p.streamTerminated = terminated;
        auto pkt = sacn::buildData(p, f.slots.data(), kDmxSlots);
        if (!pkt.ok()) return pkt.error();
        d.bytes = std::move(pkt.value());
        if (cfg_.multicast) d.dst = sacnMulticastEndpoint(p.universe);
        else { d.dst.ip = cfg_.destination; d.dst.port = kSacnPort; }
    }
    return d;
}

bool LightingEngine::transmit(const std::vector<UniverseFrame>& frames, IDatagramSink& sink, bool priority, bool terminated,
                              bool preview, std::uint32_t& count) {
    const bool isReal = (&sink == real_);
    for (std::size_t i = 0; i < frames.size() && i < kMaxUniverses; ++i) {
        auto d = build(i, frames[i], priority, terminated, preview);
        if (!d.ok()) return false;
        d.value().armToken = isReal ? armToken_ : 0;
        if (!sink.send(d.value()).ok()) return false;
        ++count;
        if (isReal) ++stats_.realDatagrams; else ++stats_.mockDatagrams;
    }
    return true;
}

void LightingEngine::disarm(Ns now) {
    if (state_ == EngineState::Armed) {
        std::vector<UniverseFrame> safe;
        resolver_.safeFrames(safe);
        std::uint32_t n = 0;
        if (real_) {
            // Best effort: if the link is gone the nodes keep their last value (documented, cannot be guaranteed).
            bool ok = true;
            for (int r = 0; r < kSafeFrameRepeats && ok; ++r) ok = transmit(safe, *real_, true, false, false, n);
            if (cfg_.protocol == LightingProtocol::Sacn)
                for (int r = 0; r < kTerminationRepeats && ok; ++r) ok = transmit(safe, *real_, true, true, false, n);
        }
        event(now, EventKind::GracefulStop, "safe frames sent: " + std::to_string(n));
        state_ = EngineState::Disarmed;
        armToken_ = 0;
        gate_ = {};
        event(now, EventKind::Disarmed, "disarmed");
    } else if (state_ == EngineState::DryRun) {
        state_ = EngineState::Disarmed;
        event(now, EventKind::Disarmed, "dry run ended");
    }
}

const TickResult& LightingEngine::tick(Ns now, const LightingInputs& in) {
    ++stats_.ticks;
    lastNow_ = now;
    result_.state = state_;
    result_.due = result_.transmitted = result_.toRealSink = result_.priorityBypass = result_.staleInputs = false;
    result_.datagrams = 0; result_.blackout = BlackoutKind::None; result_.strobeLockApplied = false;
    if (!configured_) { result_.frames.clear(); return result_; }

    // Stale generation: discard the inputs but keep honouring blackout and strobe-lock requests (safe direction).
    ResolveInputs ri;
    const bool stale = in.generation != 0 && in.generation != generation_;
    if (stale) {
        ri = lastGood_;
        ri.emergencyBlackout = ri.emergencyBlackout || in.resolve.emergencyBlackout;
        ri.lightingBlackout = ri.lightingBlackout || in.resolve.lightingBlackout;
        ri.strobingDisabled = ri.strobingDisabled || in.resolve.strobingDisabled;
        result_.staleInputs = true; ++stats_.staleInputs;
        if (!staleSeen_) { event(now, EventKind::StaleInputsDiscarded, "inputs from an older transport generation"); staleSeen_ = true; }
    } else {
        lastGood_ = in.resolve;
        ri = in.resolve;
        staleSeen_ = false;
    }
    // Priority latch: independent of the show snapshot and of the ordinary queue.
    const bool latchE = latchEmergency_.load(), latchL = latchLighting_.load();
    ri.emergencyBlackout = ri.emergencyBlackout || latchE;
    ri.lightingBlackout = ri.lightingBlackout || latchL;
    if ((latchE || latchL) != prevLatchSeen_) {
        event(now, (latchE || latchL) ? EventKind::BlackoutLatched : EventKind::BlackoutReleased,
              latchE ? "emergency blackout latched" : latchL ? "lighting blackout latched" : "blackout restored");
        prevLatchSeen_ = (latchE || latchL);
    }

    ResolveReport rep;
    resolver_.resolve(now, ri, result_.frames, &rep);
    result_.blackout = rep.blackout; result_.strobeLockApplied = rep.strobeLockApplied;
    const bool blackoutActive = rep.blackout != BlackoutKind::None;

    // ---- route: only Armed + Live reaches the real sink ----
    bool live = in.purpose == Purpose::Live && state_ == EngineState::Armed;
    if (live && (!real_ || !real_->connected())) {
        fault(now, "transport disconnected");
        live = false;
        result_.state = state_;
        // After a fault nothing is sent to the real sink; the frame is still computed for the UI.
    }
    Gate& g = gate_[live ? 0 : 1];
    const Ns period = kNsPerSec / cfg_.rateHz;
    bool due = !g.tx || now < g.last || (now - g.last) >= period;
    const bool urgent = (blackoutActive && !g.blackout) || (rep.strobeLockApplied && !g.strobe);
    if (urgent && !due) { due = true; result_.priorityBypass = true; ++stats_.priorityBypasses; }
    result_.due = due;
    if (!due) return result_;

    std::uint32_t n = 0;
    IDatagramSink& sink = live ? *real_ : static_cast<IDatagramSink&>(mock_);
    const bool ok = transmit(result_.frames, sink, blackoutActive, false, !live, n);
    result_.datagrams = n;
    result_.transmitted = n > 0;
    result_.toRealSink = live && n > 0;
    if (!ok && live) {
        fault(now, "transport send failed");
        result_.state = state_;
        return result_;
    }
    g.tx = true; g.last = now; g.blackout = blackoutActive; g.strobe = rep.strobeLockApplied;
    return result_;
}

}  // namespace ss::lighting
