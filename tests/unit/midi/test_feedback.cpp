#include "helpers.hpp"

using namespace testutil;

namespace {
constexpr Ns ms(int v) { return static_cast<Ns>(v) * kNsPerMs; }

// Engine with a controller that is both an input (port 1) and an output (port 7) with the same identity.
struct LoopRig : Rig {
    LoopRig() {
        e.openOutputPort(7, "usb:A", "Controller A", 0);
    }
};
}  // namespace

TEST_CASE("REQ-M03: feedback-risk routes require explicit enable") {
    LoopRig r;
    CHECK(r.e.isFeedbackRisk(7));
    auto s = r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().category == ErrorCategory::PermissionDenied);
    r.e.setFeedbackRouteEnabled(7, true);
    CHECK(r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0).ok());
}

TEST_CASE("REQ-M03: a route to an unrelated device is not feedback-risk and needs no enable") {
    Rig r;
    r.e.openOutputPort(9, "usb:SYNTH", "Synth", 0);
    CHECK_FALSE(r.e.isFeedbackRisk(9));
    CHECK(r.e.send(9, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0).ok());
    r.e.linkPorts(1, 9);                         // operator declares it loops back
    CHECK(r.e.isFeedbackRisk(9));
    CHECK_FALSE(r.e.send(9, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0).ok());
}

TEST_CASE("REQ-M03: origin tags forbid action->MIDI->action recursion") {
    LoopRig r;
    r.add(model::MidiMsgType::Note, 60, ActionId::NextScene);
    r.e.setFeedbackRouteEnabled(7, true);
    // an action that came from MIDI may not emit a message that the mapping on the looped-back input would accept
    auto s = r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Midi, 0);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().detail.find("recursion") != std::string::npos);
    // a different note (no mapping listens to it) is fine, e.g. LED feedback
    CHECK(r.e.send(7, makeNoteOn(0, 61, 100), 0, ActionSource::Midi, 0).ok());
    // the same note from a non-MIDI origin is allowed on an enabled route; the echo guard handles the return path
    CHECK(r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0).ok());
}

TEST_CASE("REQ-M03: triggered echo is dropped instead of re-triggering the action") {
    LoopRig r;
    r.add(model::MidiMsgType::Note, 60, ActionId::TapTempo);
    r.e.setFeedbackRouteEnabled(7, true);
    REQUIRE(r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0).ok());
    std::vector<OutPacket> sent;
    r.e.dueOutput(0, sent);
    REQUIRE(sent.size() == 1);
    r.noteOn(60, 100, ms(5));                    // the device echoes it straight back
    CHECK(r.poll(ms(6)).empty());
    CHECK(r.e.counters(1)->echoDropped == 1);
    r.noteOn(60, 100, ms(400));                  // a genuine press well after the echo window
    CHECK(r.poll(ms(401)).size() == 1);
}

TEST_CASE("REQ-M03: more than 20 identical trigger events in 1 s latch a feedback alarm and suspend the route") {
    Rig r;
    r.add(model::MidiMsgType::Note, 60, ActionId::TapTempo);
    // 20 events inside one second: legitimate, no alarm
    for (int i = 0; i < 20; ++i) r.noteOn(60, 100, ms(i * 49));
    CHECK(r.e.feedback().alarms().empty());
    CHECK(r.poll(ms(990)).size() == 20);
    // a loop: 25 identical events 20 ms apart
    Rig s;
    s.add(model::MidiMsgType::Note, 60, ActionId::TapTempo);
    for (int i = 0; i < 25; ++i) s.noteOn(60, 100, ms(i * 20));
    REQUIRE(s.e.feedback().alarms().size() == 1);
    CHECK(s.e.feedback().alarms()[0].count == 21);
    auto a = s.poll(ms(600));
    CHECK(a.size() == 20);                                   // the 21st and later were suspended
    CHECK(s.e.counters(1)->suspendedDropped == 5);
    // the route stays suspended until the operator clears the alarm
    s.noteOn(60, 100, ms(5000));
    CHECK(s.poll(ms(5001)).empty());
    REQUIRE(s.e.feedback().clearAlarm(Signature{1, model::MidiMsgType::Note, 0, 60}));
    s.noteOn(60, 100, ms(6000));
    CHECK(s.poll(ms(6001)).size() == 1);
}

TEST_CASE("REQ-M03: the loop's output half is suspended too") {
    LoopRig r;
    r.add(model::MidiMsgType::Note, 60, ActionId::TapTempo);
    r.e.setFeedbackRouteEnabled(7, true);
    // our own output of note 60 went out recently, then 21 identical inbound events arrive
    REQUIRE(r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, ms(1)).ok());
    std::vector<OutPacket> sent;
    r.e.dueOutput(ms(1), sent);
    // inbound events outside the echo window so they reach the storm detector rather than the echo filter
    for (int i = 0; i < 25; ++i) r.noteOn(60, 100, ms(300) + ms(i * 30));
    REQUIRE(r.e.feedback().alarms().size() == 1);
    CHECK(r.e.feedback().alarms()[0].outputSuspended);
    auto s = r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, ms(2000));
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().category == ErrorCategory::PermissionDenied);
    CHECK(r.e.send(7, makeNoteOn(0, 61, 100), 0, ActionSource::Show, ms(2000)).ok());    // other signatures unaffected
    r.e.feedback().clearAlarm(Signature{1, model::MidiMsgType::Note, 0, 60});
    CHECK(r.e.send(7, makeNoteOn(0, 60, 100), 0, ActionSource::Show, ms(2001)).ok());
}

TEST_CASE("REQ-M03: feedback protection does not suppress legitimate clock traffic or relative encoders") {
    Rig r;
    r.add(model::MidiMsgType::Note, 60, ActionId::TapTempo);
    MappingOptions o; o.relativeStep = 0.01;
    r.add(model::MidiMsgType::ControlChange, 16, ActionId::Brightness, model::EncoderMode::RelativeTwosComplement, 0, o);
    // trip the alarm
    for (int i = 0; i < 25; ++i) r.noteOn(60, 100, ms(i * 20));
    REQUIRE(r.e.feedback().alarms().size() == 1);
    // 24 PPQN clock at 300 BPM (120 pulses/s) keeps flowing during the alarm
    for (int i = 0; i < 240; ++i) r.e.onMessage(1, r.g, makeRealtime(MsgKind::Clock), ms(600) + i * (kNsPerSec / 120));
    CHECK(r.e.counters(1)->clockRx == 240);
    // a fast encoder sends identical +1 values far above 20/s and must not alarm
    Rig q;
    q.add(model::MidiMsgType::ControlChange, 16, ActionId::Brightness, model::EncoderMode::RelativeTwosComplement, 0, o);
    for (int i = 0; i < 200; ++i) q.cc(16, 1, i * ms(2));
    CHECK(q.e.feedback().alarms().empty());
    CHECK(q.poll(ms(500)).size() == 1);
}

TEST_CASE("REQ-M03: blackout is never suspended by a feedback alarm") {
    Rig r;
    r.add(model::MidiMsgType::Note, 60, ActionId::EmergencyBlackout);
    for (int i = 0; i < 30; ++i) r.noteOn(60, 127, ms(i * 10));
    CHECK(r.e.feedback().alarms().size() == 1);              // the loop is still reported
    auto a = r.poll(ms(400));
    REQUIRE(a.size() == 1);
    CHECK(a[0].actionId == id(ActionId::EmergencyBlackout));
    r.noteOn(60, 127, ms(1000));
    CHECK(r.poll(ms(1001)).size() == 1);                      // still reachable after the alarm
}

TEST_CASE("REQ-M03: output rate ceiling is 1000 msgs/s/port, note-off and transport are exempt") {
    OutputScheduler o;
    o.openPort(1, 0);
    int ok = 0, refused = 0;
    for (int i = 0; i < 1500; ++i) {
        auto s = o.send(1, makeCC(0, 1, static_cast<std::uint8_t>(i % 100)), 0, ActionSource::Show, i * (kNsPerSec / 1500));
        s.ok() ? ++ok : ++refused;
        if (!s.ok()) CHECK(s.error().category == ErrorCategory::ResourceExhausted);
    }
    CHECK(ok == 1000);
    CHECK(refused == 500);
    CHECK(o.stats().rateRejected == 500);
    CHECK(o.send(1, makeNoteOff(0, 60), 0, ActionSource::Show, kNsPerSec - 1).ok());
}
