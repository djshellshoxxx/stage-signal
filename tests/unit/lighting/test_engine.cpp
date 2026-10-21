// T-DMX / T-FAULT: LightingEngine state machine, isolation, blackout latch, faults, rate limiting, stale generations.
#include "doctest/doctest.h"
#include "ss/lighting/artnet.hpp"
#include "test_util.hpp"

using namespace ss;
using namespace ss::lighting;
using namespace ss::lighting::testutil;

namespace {
sacn::Cid testCid() { sacn::Cid c{}; for (std::size_t i = 0; i < 16; ++i) c[i] = static_cast<std::uint8_t>(i + 1); return c; }

LightingConfig artCfg() {
    LightingConfig c; c.protocol = LightingProtocol::ArtNet; c.destination = parseIpv4("192.168.1.50").value(); return c;
}
LightingConfig sacnCfg() {
    LightingConfig c; c.protocol = LightingProtocol::Sacn; c.destination = parseIpv4("192.168.1.60").value(); c.cid = testCid(); return c;
}
std::vector<FixturePatch> onePatch(std::uint16_t uni = 0) { return {patch(1, kProfileRgbStrobe, uni, 1)}; }

LightingInputs liveInputs() {
    LightingInputs in;
    in.resolve.cue = {look(1).set(ChannelFunction::Intensity, 1.f).set(ChannelFunction::Red, 1.f).set(ChannelFunction::Strobe, 1.f)};
    return in;
}
// Real transport stand-in: MockSink with socket-style counters.
struct Rig {
    MockSink real;
    LightingEngine eng;
    explicit Rig(const LightingConfig& cfg, std::vector<FixturePatch> patches = {}, Generation gen = 1)
        : eng(&real, gen) {
        if (patches.empty()) patches = onePatch(cfg.protocol == LightingProtocol::Sacn ? 1 : 0);   // sACN has no universe 0
        REQUIRE(eng.configure(builtinProfiles(), patches, cfg).ok());
    }
};
// Ticks every `stepMs` for `totalMs` starting at t0, returns count of ticks that transmitted.
int run(LightingEngine& e, Ns t0, int totalMs, int stepMs, const LightingInputs& in) {
    int tx = 0;
    for (int ms = 0; ms < totalMs; ms += stepMs) if (e.tick(t0 + msToNs(ms), in).transmitted) ++tx;
    return tx;
}
}  // namespace

TEST_CASE("REQ-L03: Disarmed (default) engine sends zero datagrams to the real sink") {
    Rig r(artCfg());
    CHECK(r.eng.state() == EngineState::Disarmed);
    const int tx = run(r.eng, 0, 1000, 10, liveInputs());
    CHECK(tx > 0);                                   // frames are computed and go to the mock for preview
    CHECK(r.real.datagrams() == 0);
    CHECK(r.real.attempts() == 0);
    CHECK(r.real.bytes() == 0);
    CHECK(r.eng.mockSink().datagrams() > 0);
    CHECK(r.eng.stats().realDatagrams == 0);
}

TEST_CASE("REQ-L03: dry run uses the mock sink only, even with a connected real sink") {
    Rig r(sacnCfg());
    REQUIRE(r.eng.startDryRun().ok());
    CHECK(r.eng.state() == EngineState::DryRun);
    run(r.eng, 0, 2000, 5, liveInputs());
    CHECK(r.real.attempts() == 0);
    CHECK(r.real.datagrams() == 0);
    CHECK(r.eng.mockSink().datagrams() >= 50);
    // dry-run sACN packets are flagged Preview_Data (informational only; isolation comes from the mock sink)
    auto pkt = sacn::parseData(r.eng.mockSink().log().front().bytes.data(), r.eng.mockSink().log().front().bytes.size());
    REQUIRE(pkt.ok());
    CHECK(pkt.value().params.preview);
    CHECK(r.eng.mockSink().log().front().armToken == 0);
}

TEST_CASE("REQ-L03: preview, setup and benchmark never reach the real sink even while Armed") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    for (Purpose p : {Purpose::Preview, Purpose::Setup, Purpose::Benchmark}) {
        auto in = liveInputs(); in.purpose = p;
        run(r.eng, 0, 1000, 5, in);
        CHECK(r.real.attempts() == 0);
        CHECK(r.real.datagrams() == 0);
    }
    CHECK(r.eng.mockSink().datagrams() > 0);
    // control: the same engine does reach the real sink for Live
    run(r.eng, 10 * kNsPerSec, 200, 5, liveInputs());
    CHECK(r.real.datagrams() > 0);
}

TEST_CASE("REQ-L03: preview interleaved with live traffic does not disturb live pacing or counters") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    auto prev = liveInputs(); prev.purpose = Purpose::Preview;
    int liveTx = 0;
    for (int ms = 0; ms < 1000; ++ms) {
        r.eng.tick(msToNs(ms), prev);
        if (r.eng.tick(msToNs(ms), liveInputs()).toRealSink) ++liveTx;
    }
    CHECK(liveTx == 25);
    CHECK(r.real.datagrams() == 25);
}

TEST_CASE("REQ-L04: arming rules") {
    MockSink real;
    LightingEngine e(&real);
    CHECK_FALSE(e.arm().ok());                                              // not configured
    REQUIRE(e.configure(builtinProfiles(), {patch(1, kProfileRgb, 0, 1), patch(2, kProfileDimmer, 0, 2)}, artCfg()).ok());
    auto s = e.arm();                                                       // overlapping patches
    CHECK_FALSE(s.ok());
    CHECK(s.error().category == ErrorCategory::InvalidArgument);
    CHECK(e.state() == EngineState::Disarmed);
    CHECK(e.drainEvents().back().kind == EventKind::ArmRefused);
    // dry run is still allowed with conflicts so the operator can inspect the output
    CHECK(e.startDryRun().ok());
    // resolve the conflict, then arm
    PatchOptions opt; opt.disabledPatchIds = {2};
    REQUIRE(e.configure(builtinProfiles(), {patch(1, kProfileRgb, 0, 1), patch(2, kProfileDimmer, 0, 2)}, artCfg(), opt).ok());
    real.setConnected(false);
    s = e.arm();                                                            // disconnected transport
    CHECK_FALSE(s.ok());
    CHECK(s.error().category == ErrorCategory::DeviceLost);
    real.setConnected(true);
    LightingConfig noDest = artCfg(); noDest.destination = Ipv4{};
    REQUIRE(e.configure(builtinProfiles(), {patch(1, kProfileRgb, 0, 1)}, noDest).ok());
    CHECK_FALSE(e.arm().ok());                                              // no destination and no explicit broadcast
    noDest.broadcast = true;
    REQUIRE(e.configure(builtinProfiles(), {patch(1, kProfileRgb, 0, 1)}, noDest).ok());
    CHECK(e.arm().ok());                                                    // explicit broadcast is allowed
    CHECK(e.state() == EngineState::Armed);
    CHECK(e.armToken() != 0);
    CHECK_FALSE(e.configure(builtinProfiles(), {}, artCfg()).ok());         // no reconfiguration while armed
    LightingEngine noSink(nullptr);
    REQUIRE(noSink.configure(builtinProfiles(), onePatch(), artCfg()).ok());
    CHECK(noSink.arm().error().category == ErrorCategory::UnsupportedCapability);
}

TEST_CASE("REQ-L02: configuration validation: rate, protocol-specific destination rules") {
    MockSink real; LightingEngine e(&real);
    auto c = artCfg();
    c.rateHz = 41; CHECK_FALSE(e.configure(builtinProfiles(), onePatch(), c).ok());
    c.rateHz = 0;  CHECK_FALSE(e.configure(builtinProfiles(), onePatch(), c).ok());
    c.rateHz = 40; CHECK(e.configure(builtinProfiles(), onePatch(), c).ok());
    c.rateHz = 25; c.multicast = true; CHECK_FALSE(e.configure(builtinProfiles(), onePatch(), c).ok());   // no Art-Net multicast
    auto s = sacnCfg();
    s.broadcast = true; CHECK_FALSE(e.configure(builtinProfiles(), onePatch(1), s).ok());                 // no sACN broadcast
    s.broadcast = false; s.cid = sacn::Cid{}; CHECK_FALSE(e.configure(builtinProfiles(), onePatch(1), s).ok());
    s.cid = testCid(); s.sacnPriority = 201; CHECK_FALSE(e.configure(builtinProfiles(), onePatch(1), s).ok());
    s.sacnPriority = 100; s.destination = Ipv4{};
    REQUIRE(e.configure(builtinProfiles(), onePatch(1), s).ok());
    CHECK_FALSE(e.arm().ok());                                             // neither unicast nor explicit multicast
    s.multicast = true;
    REQUIRE(e.configure(builtinProfiles(), onePatch(1), s).ok());
    CHECK(e.arm().ok());
}

TEST_CASE("REQ-L02: settings map from the show document with strict parsing") {
    model::LightingSettings st;
    st.protocol = LightingProtocol::Sacn; st.destination = "10.1.2.3"; st.rateHz = 30; st.sacnPriority = 150;
    auto c = configFromSettings(st, testCid(), "Show");
    REQUIRE(c.ok());
    CHECK(c.value().destination.toString() == "10.1.2.3");
    CHECK(c.value().sacnPriority == 150);
    CHECK(c.value().rateHz == 30);
    st.destination = "10.1.2";                     CHECK_FALSE(configFromSettings(st, testCid(), "Show").ok());
    st.destination = "10.1.2.3"; st.rateHz = 60;   CHECK_FALSE(configFromSettings(st, testCid(), "Show").ok());
    st.rateHz = 25; st.sacnPriority = 500;          CHECK_FALSE(configFromSettings(st, testCid(), "Show").ok());
    st.sacnPriority = 100; st.destination.clear();  CHECK(configFromSettings(st, testCid(), "Show").ok());
}

TEST_CASE("REQ-L02: Art-Net frames carry the patched state to the configured destination") {
    Rig r(artCfg(), {patch(1, kProfileRgbStrobe, 2, 1), patch(2, kProfileDimmer, 0, 20)});
    REQUIRE(r.eng.arm().ok());
    auto in = liveInputs();
    in.resolve.cue.push_back(look(2).set(ChannelFunction::Intensity, 0.5f));
    r.eng.tick(0, in);
    REQUIRE(r.real.datagrams() == 2);                           // one datagram per universe
    const auto& d0 = r.real.log()[0];
    CHECK(d0.dst.ip.toString() == "192.168.1.50");
    CHECK(d0.dst.port == 6454);
    CHECK(d0.armToken == r.eng.armToken());
    auto p0 = artnet::parseDmx(d0.bytes.data(), d0.bytes.size());
    REQUIRE(p0.ok());
    CHECK(p0.value().params.address.wire == 2);                 // first patch's universe leads
    CHECK(p0.value().params.sequence == 1);
    CHECK(p0.value().data.size() == 512);
    CHECK(p0.value().data[0] == 255);                           // intensity
    CHECK(p0.value().data[4] == 255);                           // strobe asked for and not locked
    auto p1 = artnet::parseDmx(r.real.log()[1].bytes.data(), r.real.log()[1].bytes.size());
    REQUIRE(p1.ok());
    CHECK(p1.value().params.address.wire == 0);
    CHECK(p1.value().data[19] == 128);
    r.eng.tick(msToNs(40), in);
    auto p2 = artnet::parseDmx(r.real.log()[2].bytes.data(), r.real.log()[2].bytes.size());
    CHECK(p2.value().params.sequence == 2);                     // sequence counts per universe
}

TEST_CASE("REQ-L02: sACN frames: priority, CID, per-universe sequence, unicast and multicast destinations") {
    auto cfg = sacnCfg(); cfg.sacnPriority = 120; cfg.sourceName = "StageSignal";
    Rig r(cfg, {patch(1, kProfileRgb, 7, 1), patch(2, kProfileRgb, 300, 1)});
    REQUIRE(r.eng.arm().ok());
    r.eng.tick(0, liveInputs());
    r.eng.tick(msToNs(40), liveInputs());
    REQUIRE(r.real.datagrams() == 4);
    auto a = sacn::parseData(r.real.log()[0].bytes.data(), r.real.log()[0].bytes.size());
    REQUIRE(a.ok());
    CHECK(a.value().params.universe.value == 7);
    CHECK(a.value().params.priority == 120);
    CHECK(a.value().params.cid == testCid());
    CHECK(a.value().params.sourceName == "StageSignal");
    CHECK_FALSE(a.value().params.preview);
    CHECK(a.value().params.sequence == 0);
    CHECK(r.real.log()[0].dst.ip.toString() == "192.168.1.60");
    CHECK(r.real.log()[0].dst.port == 5568);
    auto b = sacn::parseData(r.real.log()[2].bytes.data(), r.real.log()[2].bytes.size());
    CHECK(b.value().params.sequence == 1);
    // multicast
    cfg.multicast = true; cfg.destination = Ipv4{};
    Rig m(cfg, {patch(1, kProfileRgb, 300, 1)});
    REQUIRE(m.eng.arm().ok());
    m.eng.tick(0, liveInputs());
    CHECK(m.real.log()[0].dst.ip.toString() == "239.255.1.44");     // 300 = 0x012C
    CHECK(m.real.log()[0].dst.port == 5568);
}

TEST_CASE("REQ-L02: Art-Net discovery is opt-in") {
    auto c = artCfg();
    CHECK_FALSE(makeArtPollDatagram(c).ok());
    c.artnetDiscovery = true;
    auto d = makeArtPollDatagram(c);
    REQUIRE(d.ok());
    CHECK(d.value().dst.ip.toString() == "192.168.1.50");
    CHECK(d.value().dst.port == 6454);
    CHECK(d.value().bytes.size() == 14);
    c.destination = Ipv4{};
    CHECK(makeArtPollDatagram(c).value().dst.ip.toString() == "255.255.255.255");
    CHECK_FALSE(makeArtPollDatagram(sacnCfg()).ok());
    // the output stream never emits ArtPoll by itself
    c.artnetDiscovery = true; c.destination = parseIpv4("192.168.1.50").value();
    Rig r(c); REQUIRE(r.eng.arm().ok());
    run(r.eng, 0, 500, 5, liveInputs());
    for (const auto& dg : r.real.log()) CHECK(dg.bytes[9] == 0x50);   // ArtDmx only
}

TEST_CASE("REQ-L04: lighting blackout latch wins by the next tick and bypasses the rate gate") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    auto in = liveInputs();
    r.eng.tick(0, in);                                          // normal frame sent, gate closed for 40 ms
    CHECK(r.real.datagrams() == 1);
    const auto before = r.real.datagrams();
    r.eng.tick(msToNs(1), in);                                  // inside the gate: nothing
    CHECK(r.real.datagrams() == before);
    r.eng.latchBlackout(false);                                 // any thread
    const TickResult& t = r.eng.tick(msToNs(2), in);            // the very next tick, still inside the gate
    CHECK(t.priorityBypass);
    CHECK(t.transmitted);
    CHECK(t.blackout == BlackoutKind::Lighting);
    REQUIRE(r.real.datagrams() == before + 1);
    const auto& dg = r.real.log().back();
    CHECK(dg.priority);
    auto p = artnet::parseDmx(dg.bytes.data(), dg.bytes.size());
    REQUIRE(p.ok());
    for (int i = 0; i < 5; ++i) CHECK(p.value().data[static_cast<std::size_t>(i)] == 0);   // intensity, RGB, strobe all dark
    CHECK(r.eng.stats().priorityBypasses == 1);
    // ordinary pacing resumes: blackout frames continue at the lighting rate
    r.eng.tick(msToNs(3), in);
    CHECK(r.real.datagrams() == before + 1);
    r.eng.tick(msToNs(50), in);
    CHECK(r.real.datagrams() == before + 2);
}

TEST_CASE("REQ-L04: blackout overrides manual, cue and audio at any rate; restore is explicit") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    auto in = liveInputs();
    in.resolve.manual = {look(1).set(ChannelFunction::Intensity, 1.f)};
    r.eng.tick(0, in);
    r.eng.latchBlackout(true);                                  // emergency
    in.resolve.lightingBlackout = false;                        // the show snapshot says nothing; the latch is independent
    auto& t = r.eng.tick(msToNs(1), in);
    CHECK(t.blackout == BlackoutKind::Emergency);
    CHECK(t.frames[0].slots[0] == 0);
    CHECK(r.eng.blackoutLatched());
    // the show snapshot dropping its blackout flag does NOT restore: only restoreBlackout() does
    r.eng.tick(msToNs(100), in);
    CHECK(r.eng.resolver().sourceOf(1, 0) == Layer::EmergencyBlackout);
    r.eng.restoreBlackout();
    auto& t2 = r.eng.tick(msToNs(200), in);
    CHECK(t2.blackout == BlackoutKind::None);
    CHECK(t2.frames[0].slots[0] == 255);                        // prior look is retained and restored
    // emergency outranks lighting blackout when both asserted
    r.eng.latchBlackout(false); r.eng.latchBlackout(true);
    CHECK(r.eng.tick(msToNs(300), in).blackout == BlackoutKind::Emergency);
    auto ev = r.eng.drainEvents();
    int latched = 0, released = 0;
    for (const auto& e : ev) { if (e.kind == EventKind::BlackoutLatched) ++latched; if (e.kind == EventKind::BlackoutReleased) ++released; }
    CHECK(latched == 2); CHECK(released == 1);
}

TEST_CASE("REQ-L04: blackout works from the show snapshot flags too and also in Disarmed/DryRun mock output") {
    Rig r(artCfg());
    auto in = liveInputs();
    in.resolve.lightingBlackout = true;
    auto& t = r.eng.tick(0, in);                                // Disarmed
    CHECK(t.blackout == BlackoutKind::Lighting);
    CHECK(t.frames[0].slots[1] == 0);
    CHECK(r.real.attempts() == 0);
}

TEST_CASE("REQ-L04: blackout frames use fixture safe values, not all zero") {
    auto cfg = artCfg();
    MockSink real; LightingEngine e(&real);
    REQUIRE(e.configure({movingHead()}, {patch(1, "head", 0, 1)}, cfg).ok());
    REQUIRE(e.arm().ok());
    LightingInputs in; in.resolve.lightingBlackout = true;
    e.tick(0, in);
    auto p = artnet::parseDmx(real.log().back().bytes.data(), real.log().back().bytes.size());
    REQUIRE(p.ok());
    CHECK(p.value().data[0] == 128);                            // pan safe
    CHECK(p.value().data[2] == 100);                            // tilt safe
    CHECK(p.value().data[3] == 0);                              // intensity
    CHECK(p.value().data[5] == 7);                              // macro safe value
}

TEST_CASE("REQ-S12: strobe lock through the engine bypasses the rate gate and zeroes strobe") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    auto in = liveInputs();
    r.eng.tick(0, in);
    CHECK(r.real.log()[0].bytes[18 + 4] == 255);                // strobe active
    in.resolve.strobingDisabled = true;
    auto& t = r.eng.tick(msToNs(1), in);
    CHECK(t.priorityBypass);
    CHECK(t.strobeLockApplied);
    CHECK(r.real.log().back().bytes[18 + 4] == 0);
    CHECK(r.real.log().back().bytes[18 + 1] == 255);            // red keeps going
    in.resolve.strobingDisabled = false;
    r.eng.tick(msToNs(50), in);
    CHECK(r.real.log().back().bytes[18 + 4] == 255);
}

TEST_CASE("REQ-L05: send failure is reported, engine disarms, no further real datagrams") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    run(r.eng, 0, 200, 5, liveInputs());
    const auto sentBefore = r.real.datagrams();
    CHECK(sentBefore > 0);
    r.real.failAfter(0);                                        // next send fails
    auto& t = r.eng.tick(msToNs(1000), liveInputs());
    CHECK(r.eng.state() == EngineState::Disarmed);
    CHECK(r.eng.needsExplicitArm());
    CHECK_FALSE(t.toRealSink);
    CHECK(t.state == EngineState::Disarmed);
    CHECK(r.eng.stats().faults == 1);
    auto ev = r.eng.drainEvents();
    bool fault = false; for (const auto& e : ev) if (e.kind == EventKind::TransportFault) fault = true;
    CHECK(fault);
    const auto attempts = r.real.attempts();
    run(r.eng, msToNs(2000), 1000, 5, liveInputs());
    CHECK(r.real.attempts() == attempts);                       // nothing further touches the real sink
    CHECK(r.real.datagrams() == sentBefore);
}

TEST_CASE("REQ-L05: disconnect is detected, reconnect leaves the engine disarmed until explicit Arm") {
    Rig r(sacnCfg(), onePatch(1));
    REQUIRE(r.eng.arm().ok());
    run(r.eng, 0, 200, 5, liveInputs());
    r.real.setConnected(false);
    r.eng.tick(msToNs(300), liveInputs());
    CHECK(r.eng.state() == EngineState::Disarmed);
    CHECK(r.eng.stats().faults == 1);
    // while disconnected a manual Arm is refused
    CHECK(r.eng.arm().error().category == ErrorCategory::DeviceLost);
    // transport comes back with a new generation
    r.real.setConnected(true);
    r.eng.transportReconnected(msToNs(400), 2);
    CHECK(r.eng.generation() == 2);
    CHECK(r.eng.state() == EngineState::Disarmed);
    const auto sent = r.real.datagrams();
    const auto attempts = r.real.attempts();
    auto in = liveInputs(); in.generation = 2;
    run(r.eng, msToNs(500), 1000, 5, in);
    CHECK(r.eng.state() == EngineState::Disarmed);              // stays disarmed, no automatic re-arm
    CHECK(r.real.datagrams() == sent);
    CHECK(r.real.attempts() == attempts);
    REQUIRE(r.eng.arm().ok());                                  // only explicit Arm resumes output
    CHECK(r.eng.needsExplicitArm() == false);
    r.eng.tick(msToNs(2000), in);
    CHECK(r.real.datagrams() == sent + 1);
}

TEST_CASE("REQ-L05: reconnect notification while still Armed also forces Disarmed; app-reported loss is a fault") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    r.eng.reportTransportLost(0, "interface down");
    CHECK(r.eng.state() == EngineState::Disarmed);
    CHECK(r.eng.needsExplicitArm());
    REQUIRE(r.eng.arm().ok());
    r.eng.transportReconnected(msToNs(10), 5);
    CHECK(r.eng.state() == EngineState::Disarmed);
    CHECK(r.eng.armToken() == 0);
}

TEST_CASE("REQ-L05: graceful stop sends safe frames and sACN stream termination") {
    Rig r(sacnCfg(), {patch(1, "head", 5, 1), patch(2, "head", 6, 1)});
    // use the moving head so safe values are visible; re-configure with its profile
    REQUIRE(r.eng.configure({movingHead()}, {patch(1, "head", 5, 1), patch(2, "head", 6, 1)}, sacnCfg()).ok());
    REQUIRE(r.eng.arm().ok());
    LightingInputs in;
    in.resolve.cue = {look(1).set(ChannelFunction::Intensity, 1.f), look(2).set(ChannelFunction::Intensity, 1.f)};
    r.eng.tick(0, in);
    const auto live = r.real.datagrams();
    CHECK(live == 2);
    r.eng.stop(msToNs(100));
    CHECK(r.eng.state() == EngineState::Disarmed);
    // 3 safe repeats + 3 termination repeats, per universe
    CHECK(r.real.datagrams() == live + 2 * (kSafeFrameRepeats + kTerminationRepeats));
    int terminated = 0;
    for (std::size_t i = live; i < r.real.log().size(); ++i) {
        const auto& d = r.real.log()[i];
        CHECK(d.priority);
        auto p = sacn::parseData(d.bytes.data(), d.bytes.size());
        REQUIRE(p.ok());
        CHECK(p.value().data[3] == 0);                           // intensity dark
        CHECK(p.value().data[0] == 128);                         // pan safe value
        CHECK(p.value().data[2] == 100);                         // tilt safe value
        if (p.value().params.streamTerminated) ++terminated;
    }
    CHECK(terminated == 2 * kTerminationRepeats);
    // the very last packets are terminations
    auto last = sacn::parseData(r.real.log().back().bytes.data(), r.real.log().back().bytes.size());
    CHECK(last.value().params.streamTerminated);
    // further ticks do not touch the real sink
    const auto n = r.real.datagrams();
    run(r.eng, msToNs(200), 500, 5, in);
    CHECK(r.real.datagrams() == n);
}

TEST_CASE("REQ-L05: graceful stop on Art-Net sends safe frames only (no termination in the protocol)") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    r.eng.tick(0, liveInputs());
    r.eng.disarm(msToNs(10));
    CHECK(r.real.datagrams() == 1 + kSafeFrameRepeats);
    auto p = artnet::parseDmx(r.real.log().back().bytes.data(), r.real.log().back().bytes.size());
    CHECK(p.value().data[0] == 0);
    CHECK(p.value().data[4] == 0);
}

TEST_CASE("REQ-L05: stop from Disarmed or DryRun transmits nothing live") {
    Rig r(artCfg());
    r.eng.stop(0);
    REQUIRE(r.eng.startDryRun().ok());
    run(r.eng, 0, 100, 5, liveInputs());
    r.eng.stop(msToNs(200));
    CHECK(r.eng.state() == EngineState::Disarmed);
    CHECK(r.real.attempts() == 0);
}

TEST_CASE("REQ-L02: rate limiting: 25 Hz default and 40 Hz maximum, deterministic") {
    {
        Rig r(artCfg());
        REQUIRE(r.eng.arm().ok());
        CHECK(run(r.eng, 0, 1000, 1, liveInputs()) == 25);
        CHECK(r.real.datagrams() == 25);
    }
    {
        auto c = artCfg(); c.rateHz = 40;
        Rig r(c);
        REQUIRE(r.eng.arm().ok());
        CHECK(run(r.eng, 0, 1000, 1, liveInputs()) == 40);
    }
    {   // a slow caller never double-sends: 100 ms ticks at 25 Hz give 10 frames per second
        Rig r(artCfg());
        REQUIRE(r.eng.arm().ok());
        CHECK(run(r.eng, 0, 1000, 100, liveInputs()) == 10);
    }
    {   // a clock regression re-opens the gate instead of freezing output
        Rig r(artCfg());
        REQUIRE(r.eng.arm().ok());
        r.eng.tick(msToNs(1000), liveInputs());
        CHECK(r.eng.tick(msToNs(10), liveInputs()).transmitted);
    }
}

TEST_CASE("REQ-L02: latest-state frames are returned every tick even when the rate gate is closed") {
    Rig r(artCfg());
    REQUIRE(r.eng.arm().ok());
    auto in = liveInputs();
    r.eng.tick(0, in);
    in.resolve.cue = {look(1).set(ChannelFunction::Red, 0.f).set(ChannelFunction::Intensity, 0.5f)};
    auto& t = r.eng.tick(msToNs(5), in);
    CHECK_FALSE(t.due);
    CHECK_FALSE(t.transmitted);
    REQUIRE(t.frames.size() == 1);
    CHECK(t.frames[0].slots[0] == 128);                         // the freshest state, not the stale one
    auto& t2 = r.eng.tick(msToNs(41), in);
    CHECK(t2.transmitted);
    auto p = artnet::parseDmx(r.real.log().back().bytes.data(), r.real.log().back().bytes.size());
    CHECK(p.value().data[0] == 128);                            // coalesced: the intermediate state was never queued
}

TEST_CASE("REQ-L05: stale generations are discarded") {
    Rig r(artCfg(), {}, 3);
    REQUIRE(r.eng.arm().ok());
    auto good = liveInputs(); good.generation = 3;
    r.eng.tick(0, good);
    // inputs built for an older transport generation are ignored; the last good look is reused
    auto stale = liveInputs(); stale.generation = 2;
    stale.resolve.cue = {look(1).set(ChannelFunction::Intensity, 0.f)};
    auto& t = r.eng.tick(msToNs(50), stale);
    CHECK(t.staleInputs);
    CHECK(t.frames[0].slots[0] == 255);
    CHECK(r.eng.stats().staleInputs == 1);
    // ... but a stale snapshot can still only make things SAFER: blackout and strobe lock are honoured
    stale.resolve.lightingBlackout = true;
    auto& t2 = r.eng.tick(msToNs(100), stale);
    CHECK(t2.blackout == BlackoutKind::Lighting);
    CHECK(t2.frames[0].slots[0] == 0);
    // sink-level: datagrams of an old generation are discarded, not sent
    MockSink sink; sink.setExpectedGeneration(4);
    Datagram d; d.generation = 3; d.bytes = {1};
    CHECK(sink.send(d).ok());
    CHECK(sink.datagrams() == 0);
    CHECK(sink.staleDiscarded() == 1);
    d.generation = 4;
    CHECK(sink.send(d).ok());
    CHECK(sink.datagrams() == 1);
    // unstamped inputs (generation 0) are accepted
    auto unstamped = liveInputs();
    CHECK_FALSE(r.eng.tick(msToNs(200), unstamped).staleInputs);
}

TEST_CASE("REQ-L04: determinism: identical inputs give identical datagram bytes") {
    auto drive = [](Rig& r) {
        REQUIRE(r.eng.arm().ok());
        auto in = liveInputs();
        for (int i = 0; i < 100; ++i) {
            in.resolve.cue = {look(1).set(ChannelFunction::Red, static_cast<float>(i) / 100.f)};
            r.eng.tick(msToNs(i * 40), in);
        }
    };
    Rig a(sacnCfg(), onePatch(1)), b(sacnCfg(), onePatch(1));
    drive(a); drive(b);
    REQUIRE(a.real.log().size() == b.real.log().size());
    for (std::size_t i = 0; i < a.real.log().size(); ++i) CHECK(a.real.log()[i].bytes == b.real.log()[i].bytes);
}

TEST_CASE("REQ-L02: universe boundary patches produce valid wire packets") {
    {   // Art-Net universe 0 and 32767
        Rig r(artCfg(), {patch(1, kProfileDimmer, 0, 1), patch(2, kProfileDimmer, 32767, 1)});
        REQUIRE(r.eng.arm().ok());
        r.eng.tick(0, liveInputs());
        CHECK(artnet::parseDmx(r.real.log()[0].bytes.data(), r.real.log()[0].bytes.size()).value().params.address.wire == 0);
        CHECK(artnet::parseDmx(r.real.log()[1].bytes.data(), r.real.log()[1].bytes.size()).value().params.address.wire == 32767);
    }
    {   // sACN universe 1 and 63999
        Rig r(sacnCfg(), {patch(1, kProfileDimmer, 1, 1), patch(2, kProfileDimmer, 63999, 512)});
        REQUIRE(r.eng.arm().ok());
        r.eng.tick(0, liveInputs());
        CHECK(sacn::parseData(r.real.log()[0].bytes.data(), r.real.log()[0].bytes.size()).value().params.universe.value == 1);
        CHECK(sacn::parseData(r.real.log()[1].bytes.data(), r.real.log()[1].bytes.size()).value().params.universe.value == 63999);
    }
    {   // the same integer is wrong in the other protocol and cannot arm
        MockSink real; LightingEngine e(&real);
        REQUIRE(e.configure(builtinProfiles(), {patch(1, kProfileDimmer, 0, 1)}, sacnCfg()).ok());
        CHECK_FALSE(e.arm().ok());                              // sACN has no universe 0
    }
}

TEST_CASE("REQ-L03: event log and stats are bounded") {
    Rig r(artCfg());
    for (int i = 0; i < 200; ++i) { r.eng.latchBlackout(false); r.eng.tick(msToNs(i * 2), liveInputs()); r.eng.restoreBlackout(); r.eng.tick(msToNs(i * 2 + 1), liveInputs()); }
    auto ev = r.eng.drainEvents();
    CHECK(ev.size() <= kMaxEvents);
    CHECK(r.eng.stats().droppedEvents > 0);
}
