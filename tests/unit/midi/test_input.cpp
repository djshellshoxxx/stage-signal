#include "helpers.hpp"

using namespace testutil;

namespace {
constexpr Ns ms(int v) { return static_cast<Ns>(v) * kNsPerMs; }
}

TEST_CASE("REQ-D11: brightness, speed, intensity, colour and next-scene reach actions through MIDI mappings") {
    Rig r;
    r.add(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    r.add(model::MidiMsgType::ControlChange, 2, ActionId::Speed);
    r.add(model::MidiMsgType::ControlChange, 3, ActionId::Intensity);
    r.add(model::MidiMsgType::ControlChange, 4, ActionId::ColorHue);
    r.add(model::MidiMsgType::ControlChange, 5, ActionId::ColorSaturation);
    r.add(model::MidiMsgType::Note, 36, ActionId::NextScene);
    for (int n = 1; n <= 5; ++n) r.cc(static_cast<std::uint8_t>(n), 127, ms(n));
    r.noteOn(36, 100, ms(10));
    auto a = r.poll(ms(11));
    REQUIRE(a.size() == 6);
    const ActionId expect[] = {ActionId::Brightness, ActionId::Speed, ActionId::Intensity, ActionId::ColorHue,
                               ActionId::ColorSaturation, ActionId::NextScene};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(a[i].actionId == id(expect[i]));
        CHECK(a[i].source == ActionSource::Midi);               // origin tag
    }
    CHECK(a[0].value == doctest::Approx(1.0));
}

TEST_CASE("REQ-M01: SceneSelect carries its slot, SetBpm carries BPM, CC half-range maps linearly") {
    Rig r;
    MappingOptions slot3; slot3.triggerValue = 3;
    r.add(model::MidiMsgType::Note, 40, ActionId::SceneSelect, model::EncoderMode::Absolute, 0, slot3);
    r.add(model::MidiMsgType::ControlChange, 9, ActionId::SetBpm);
    r.noteOn(40, 90, 0);
    r.cc(9, 0, 1); r.cc(9, 127, 2);
    auto a = r.poll(ms(3));
    REQUIRE(a.size() == 2);
    CHECK(a[0].actionId == id(ActionId::SceneSelect));
    CHECK(a[0].value == 3.0);
    CHECK(a[1].actionId == id(ActionId::SetBpm));
    CHECK(a[1].value == doctest::Approx(kMaxBpm));              // latest value won; scaled to BPM units
}

TEST_CASE("REQ-M01: note-off and zero-velocity note-on do not fire triggers") {
    Rig r;
    r.add(model::MidiMsgType::Note, 36, ActionId::NextScene);
    r.noteOff(36, 0);
    r.noteOn(36, 0, ms(100));                                    // velocity 0 == note-off
    CHECK(r.poll(ms(101)).empty());
    r.e.onBytes(1, r.g, std::vector<std::uint8_t>{0x90, 36, 0}.data(), 3, ms(200));
    CHECK(r.poll(ms(201)).empty());
    r.e.onBytes(1, r.g, std::vector<std::uint8_t>{0x90, 36, 100}.data(), 3, ms(300));
    CHECK(r.poll(ms(301)).size() == 1);
}

TEST_CASE("REQ-M01: program change and CC edge triggers") {
    Rig r;
    r.add(model::MidiMsgType::ProgramChange, 7, ActionId::NextScene);
    r.add(model::MidiMsgType::ControlChange, 64, ActionId::TapTempo);       // footswitch CC
    r.pc(7, 0);
    r.cc(64, 127, ms(1)); r.cc(64, 127, ms(2));                  // held: one edge only
    r.cc(64, 0, ms(3)); r.cc(64, 127, ms(4));                    // release then press again
    auto a = r.poll(ms(5));
    REQUIRE(a.size() == 3);
    CHECK(a[0].actionId == id(ActionId::NextScene));
    CHECK(a[1].actionId == id(ActionId::TapTempo));
    CHECK(a[2].actionId == id(ActionId::TapTempo));
}

TEST_CASE("REQ-M01: scene triggers are debounced 50 ms, other triggers are not") {
    Rig r;
    r.add(model::MidiMsgType::Note, 36, ActionId::NextScene);
    r.add(model::MidiMsgType::Note, 37, ActionId::TapTempo);
    r.noteOn(36, 100, 0);
    r.noteOn(36, 100, ms(30));        // inside 50 ms: dropped
    r.noteOn(36, 100, ms(49));        // still inside (measured from the accepted one)
    r.noteOn(36, 100, ms(60));        // accepted
    r.noteOn(37, 100, ms(61));
    r.noteOn(37, 100, ms(62));
    auto a = r.poll(ms(63));
    int next = 0, tap = 0;
    for (auto& x : a) { next += x.actionId == id(ActionId::NextScene); tap += x.actionId == id(ActionId::TapTempo); }
    CHECK(next == 2);
    CHECK(tap == 2);
    CHECK(r.e.totals().debounced == 2);
}

TEST_CASE("REQ-M01: continuous controls coalesce to the latest value") {
    Rig r;
    r.add(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    for (int v = 0; v <= 100; ++v) r.cc(1, static_cast<std::uint8_t>(v), ms(v));      // 101 messages
    CHECK(r.e.queued() == 1);
    auto a = r.poll(ms(200));
    REQUIRE(a.size() == 1);
    CHECK(a[0].value == doctest::Approx(100.0 / 127.0));
    CHECK(r.e.queueCounters().coalesced == 100);
    CHECK(r.e.queueCounters().lost() == 0);                      // coalescing is not loss
}

TEST_CASE("REQ-M01: the final release of a continuous control is retained") {
    Rig r;
    r.add(model::MidiMsgType::Note, 50, ActionId::Intensity);   // pad held = intensity
    r.noteOn(50, 127, 0);
    r.noteOff(50, ms(5));
    auto a = r.poll(ms(6));
    REQUIRE(a.size() == 1);
    CHECK(a[0].value == doctest::Approx(0.0));
}

TEST_CASE("REQ-M04: queue of 512 drops stale continuous values first and never the blackout latch") {
    Rig r;
    // 200 distinct continuous mappings (fit under the 512 mapping limit), plus triggers
    for (int i = 0; i < 100; ++i)
        r.add(model::MidiMsgType::ControlChange, static_cast<std::uint8_t>(i), ActionId::Brightness);
    r.add(model::MidiMsgType::Note, 1, ActionId::TapTempo);
    r.add(model::MidiMsgType::Note, 100, ActionId::EmergencyBlackout);
    for (int i = 0; i < 100; ++i) r.cc(static_cast<std::uint8_t>(i), 64, ms(i));
    CHECK(r.e.queued() == 100);
    // 520 trigger events overflow the 512 queue (staying under the 1000/s ceiling)
    for (int i = 0; i < 520; ++i) {
        r.noteOn(1, static_cast<std::uint8_t>(1 + i % 100), ms(200) + i * 100 * 1000);      // 100 us spacing
        r.e.feedback().clearAlarm(Signature{1, model::MidiMsgType::Note, 0, 1});            // keep the storm latch out of this test
    }
    const auto& qc = r.e.queueCounters();
    CHECK(qc.droppedContinuous == 100);              // all stale continuous values were sacrificed first
    CHECK(qc.droppedDiscrete > 0);
    r.noteOn(100, 127, ms(500));                     // emergency blackout while the queue is full
    auto a = r.poll(ms(501));
    REQUIRE(!a.empty());
    CHECK(a.front().actionId == id(ActionId::EmergencyBlackout));   // latch drains before the queue
    CHECK(a.size() <= 1 + kEventQueueCapacity);
}

TEST_CASE("REQ-M04: 1000 msgs/s/port rate ceiling with a diagnostic counter; clock is not suppressed") {
    Rig r;
    r.add(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    // 1500 CC in one second
    for (int i = 0; i < 1500; ++i) r.cc(1, static_cast<std::uint8_t>(i % 100), i * (kNsPerSec / 1500));
    CHECK(r.e.counters(1)->rateDropped == 500);
    // heavy clock traffic (6 pulses per ms) is counted, never dropped, and does not consume the ceiling
    Rig c;
    c.add(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    for (int i = 0; i < 5000; ++i) c.e.onMessage(1, c.g, makeRealtime(MsgKind::Clock), i * 100000LL);
    CHECK(c.e.counters(1)->clockRx == 5000);
    CHECK(c.e.counters(1)->rateDropped == 0);
    c.cc(1, 10, kNsPerSec);
    CHECK(c.e.counters(1)->rateDropped == 0);
    CHECK(c.poll(kNsPerSec + 1).size() == 1);
}

TEST_CASE("REQ-M04: blackout bypasses the rate ceiling") {
    Rig r;
    r.add(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    r.add(model::MidiMsgType::Note, 5, ActionId::EmergencyBlackout);
    for (int i = 0; i < 1000; ++i) r.cc(1, 1, i * 1000);                     // saturate the ceiling inside 1 ms * 1000
    r.noteOn(5, 127, 1000 * 1000);
    CHECK(r.e.counters(1)->rateCriticalBypass == 1);
    auto a = r.poll(ms(2));
    REQUIRE(a.size() >= 1);
    CHECK(a.front().actionId == id(ActionId::EmergencyBlackout));
}

TEST_CASE("REQ-M04: lost connection marks mappings unavailable within 1 s of detection") {
    Rig r;
    const auto mapId = r.add(model::MidiMsgType::Note, 36, ActionId::NextScene);
    CHECK(r.e.mappingStatus(mapId).availability == MappingAvailability::Available);
    // unplug at t=10 s; the adapter enumeration poll (500 ms) notices at t=10.5 s
    const Ns unplug = 10 * kNsPerSec;
    const Ns detected = unplug + kDevicePollPeriodNs;
    r.e.updateInputDevices({PortInfo{1, "usb:A", "Controller A", 0, true, 0, true}}, unplug + ms(250));   // still present
    CHECK(r.e.mappingStatus(mapId).usable());
    r.e.updateInputDevices({}, detected);
    const MappingStatus st = r.e.mappingStatus(mapId);
    CHECK(st.availability == MappingAvailability::DeviceUnavailable);
    CHECK(st.unavailableSince - unplug <= kLossMarkDeadlineNs);              // within 1 s of the actual loss
    CHECK(st.unavailableSince == detected);                                   // and immediately on detection
    // messages for the dead generation are ignored
    r.noteOn(36, 100, detected + ms(1));
    CHECK(r.poll(detected + ms(2)).empty());
}

TEST_CASE("REQ-M04: reconnect clears the old generation and sends no stale queued actions") {
    Rig r;
    r.add(model::MidiMsgType::Note, 36, ActionId::NextScene);
    r.add(model::MidiMsgType::Note, 37, ActionId::TapTempo);
    r.add(model::MidiMsgType::ControlChange, 1, ActionId::Brightness);
    r.add(model::MidiMsgType::Note, 38, ActionId::RestoreBlackout);
    r.add(model::MidiMsgType::Note, 39, ActionId::EmergencyBlackout);
    const Generation old = r.g;
    r.noteOn(36, 100, 0); r.noteOn(37, 100, ms(1)); r.cc(1, 99, ms(2));
    r.noteOn(38, 100, ms(3));                       // unsafe-direction latch entry
    r.noteOn(39, 100, ms(4));                       // safe-direction latch entry (blackout)
    CHECK(r.e.queued() == 5);
    r.e.inputPortLost(1, ms(500));
    r.g = r.e.openInputPort(1, "usb:A", "Controller A", ms(900));
    CHECK(r.g != old);
    auto a = r.poll(ms(901));
    REQUIRE(a.size() == 1);                         // only the blackout survives; the rest is stale
    CHECK(a[0].actionId == id(ActionId::EmergencyBlackout));
    // a late callback from the old generation is rejected
    r.e.onMessage(1, old, makeNoteOn(0, 36, 100), ms(950));
    CHECK(r.poll(ms(951)).empty());
    CHECK(r.e.counters(1)->staleGeneration >= 1);
    // the new generation works
    r.noteOn(36, 100, ms(1000));
    CHECK(r.poll(ms(1001)).size() == 1);
}

TEST_CASE("REQ-M04: PriorityLatch collapses repeats and drains emergency first") {
    PriorityLatch l;
    Action a; a.actionId = id(ActionId::VisualBlackout);
    Action b; b.actionId = id(ActionId::EmergencyBlackout);
    Action c; c.actionId = id(ActionId::DisarmLighting);
    l.set(a, 1, 1); l.set(c, 1, 1); l.set(b, 1, 1); l.set(a, 1, 1);
    CHECK(l.size() == 3);
    std::vector<Action> out;
    l.drain(out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].actionId == id(ActionId::EmergencyBlackout));
    CHECK(l.empty());
}

TEST_CASE("REQ-M04: PendingQueue direct overflow policy") {
    PendingQueue q(4);
    auto mk = [](std::uint32_t mapId, bool cont) { Pending p; p.mappingId = mapId; p.portId = 1; p.continuous = cont; p.action.actionId = mapId; return p; };
    q.pushContinuous(mk(1, true)); q.pushDiscrete(mk(2, false)); q.pushContinuous(mk(3, true)); q.pushDiscrete(mk(4, false));
    q.pushDiscrete(mk(5, false));          // full: oldest continuous (1) goes
    CHECK(q.counters().droppedContinuous == 1);
    q.pushDiscrete(mk(6, false));          // next continuous (3) goes
    CHECK(q.counters().droppedContinuous == 2);
    q.pushDiscrete(mk(7, false));          // all discrete now: oldest discrete (2) goes
    CHECK(q.counters().droppedDiscrete == 1);
    std::vector<Pending> out;
    q.drain(out);
    REQUIRE(out.size() == 4);
    CHECK(out.front().mappingId == 4);
    CHECK(out.back().mappingId == 7);
}

TEST_CASE("REQ-M04: critical actions are never debounced") {
    Rig r;
    r.add(model::MidiMsgType::Note, 5, ActionId::EmergencyBlackout);
    r.noteOn(5, 127, 0); r.noteOn(5, 127, ms(5)); r.noteOn(5, 127, ms(10));
    CHECK(r.e.totals().debounced == 0);
    CHECK(r.poll(ms(11)).size() == 1);                        // latched slot collapses repeated presses
}
