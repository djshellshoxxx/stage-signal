#include "test.h"
#include "midi/router.h"
#include "midi/mock_port.h"
using namespace ss;

namespace {
struct Rig {
  MappingTable t; MidiRouteGraph g; FakeClock clk; MidiRouter r{t, g, clk};
  std::vector<Action> got; int p = 0;
  Rig() { p = r.addPort("dev", "Dev"); r.setActionSink([this](const Action& a) { got.push_back(a); }); }
  std::string map(MidiMsgType ty, int num, ActionId a, int ch = -1) {
    MidiMapping m; m.deviceId = "dev"; m.type = ty; m.number = num; m.channel = ch; m.action = a; m.modeConfirmed = true;
    return t.add(m);
  }
  void note(int n, int vel, Ns at, int ch = 0) { uint8_t b[3] = {(uint8_t)(0x90 | ch), (uint8_t)n, (uint8_t)vel}; r.onBytes(p, at, b, 3); }
  void cc(int n, int v, Ns at, int ch = 0) { uint8_t b[3] = {(uint8_t)(0xB0 | ch), (uint8_t)n, (uint8_t)v}; r.onBytes(p, at, b, 3); }
};
}

TEST(midi_router_basic_origin_tag) {
  Rig g; g.map(MidiMsgType::Note, 60, ActionId::NextScene); g.map(MidiMsgType::CC, 7, ActionId::Brightness);
  g.note(60, 100, 0); g.cc(7, 127, 1000);
  CHECK(g.r.pump() == 2 && g.got.size() == 2);
  CHECK(g.got[0].id == ActionId::NextScene && g.got[0].source == ActionSource::Midi);
  CHECK(g.got[1].id == ActionId::Brightness && g.got[1].value == 1.0);
  CHECK(isMidiOriginated(g.got[0]) && !actionMayEmitMidi(g.got[0]));
  Action ui; ui.source = ActionSource::UI; CHECK(actionMayEmitMidi(ui));
  Action sneaky; sneaky.source = ActionSource::Show; sneaky.correlation = g.got[0].correlation; CHECK(!actionMayEmitMidi(sneaky));
  g.note(61, 100, 2000); CHECK(g.r.pump() == 0 && g.r.stats().unmapped == 1);
}

TEST(midi_router_scene_debounce_50ms) {
  Rig g; g.map(MidiMsgType::Note, 60, ActionId::NextScene);
  g.note(60, 100, 0); g.note(60, 101, 20 * kNsPerMs); g.note(60, 102, 49 * kNsPerMs);
  CHECK(g.r.pump() == 1);                                        // leading edge only
  g.note(60, 103, 60 * kNsPerMs);
  CHECK(g.r.pump() == 1 && g.got.size() == 2);                   // >=50 ms since last accepted
  CHECK(g.r.stats().debounced == 2);
  g.note(60, 104, 70 * kNsPerMs);
  CHECK(g.r.pump() == 0);
}

TEST(midi_router_continuous_coalesce_keeps_critical) {
  Rig g; g.map(MidiMsgType::CC, 7, ActionId::Brightness); g.map(MidiMsgType::CC, 8, ActionId::Speed);
  g.map(MidiMsgType::Note, 1, ActionId::EmergencyBlackout); g.map(MidiMsgType::Note, 2, ActionId::StopRecording);
  g.map(MidiMsgType::Note, 3, ActionId::NextScene);
  for (int i = 0; i < 100; ++i) { g.cc(7, i, i * 1000); if (i % 10 == 0) g.cc(8, 127 - i, i * 1000 + 1); }
  g.note(1, 127, 200000); g.note(2, 127, 200001); g.note(3, 127, 200002);
  CHECK(g.r.stats().queueDepth == 3);                            // 1 coalesced CC7 + CC8 + NextScene (latch separate)
  size_t n = g.r.pump();
  CHECK(n == 5);
  CHECK(g.got[0].id == ActionId::EmergencyBlackout && g.got[1].id == ActionId::StopRecording);   // latch first
  int b = 0, s = 0; double bv = -1, sv = -1;
  for (auto& a : g.got) { if (a.id == ActionId::Brightness) { ++b; bv = a.value; } if (a.id == ActionId::Speed) { ++s; sv = a.value; } }
  CHECK(b == 1 && s == 1);
  CHECK_NEAR(bv, 99 / 127.0, 1e-9); CHECK_NEAR(sv, (127 - 90) / 127.0, 1e-9);
}

TEST(midi_router_relative_accumulates_not_replaced) {
  Rig g; MidiMapping m; m.deviceId = "dev"; m.type = MidiMsgType::CC; m.number = 5; m.action = ActionId::Intensity;
  m.mode = MidiMode::RelativeBinaryOffset; m.modeConfirmed = true; m.relStep = 0.01; g.t.add(m);
  for (int i = 0; i < 10; ++i) g.cc(5, 65, i * 1000);
  CHECK(g.r.stats().queueDepth == 10);
  CHECK(g.r.pump() == 1);
  CHECK_NEAR(g.got[0].value, 0.6, 1e-9);                         // all ten ticks applied
}

TEST(midi_router_port_loss_reconnect_no_stale) {
  FakeClock clk; MappingTable t; MidiRouteGraph gr; MidiRouter r(t, gr, clk); MockMidiPort port(&clk);
  std::vector<Action> got; r.setActionSink([&](const Action& a) { got.push_back(a); });
  std::vector<MidiAlarm> alarms; r.setAlarmCallback([&](const MidiAlarm& a) { alarms.push_back(a); });
  MidiPortInfo info; info.stableId = "dev"; info.name = "Dev"; info.input = true;
  port.addPort(info);
  int idx = -1; CHECK(r.attach(port, info, &idx).ok());
  MidiMapping m; m.deviceId = "dev"; m.type = MidiMsgType::Note; m.number = 60; m.action = ActionId::NextScene; std::string id = t.add(m);
  MidiMapping c; c.deviceId = "dev"; c.type = MidiMsgType::Note; c.number = 1; c.action = ActionId::EmergencyBlackout; t.add(c);
  CHECK(t.status(id) == MappingStatus::Active);
  port.injectInput({0x90, 60, 100}, 0); port.injectInput({0x90, 1, 100}, 1);   // queued + latched, not yet pumped
  CHECK(r.stats().queueDepth == 1);
  uint32_t gen0 = r.portGeneration(idx);
  port.simulateLoss(true);                                       // lost callback -> unavailable immediately
  CHECK(t.status(id) == MappingStatus::DeviceUnavailable);
  CHECK(t.unavailableMappings().size() == 2);
  CHECK(r.stats().queueDepth == 0 && r.portGeneration(idx) != gen0);
  CHECK(!port.injectInput({0x90, 60, 100}, 5));                  // dead port
  CHECK(r.pump() == 0 && got.empty());                           // nothing stale after loss
  bool sawLost = false; for (auto& a : alarms) if (a.kind == MidiAlarmKind::PortLost) sawLost = true; CHECK(sawLost);
  // direct injection through router while lost is ignored
  uint8_t b[3] = {0x90, 60, 100}; r.onBytes(idx, 10, b, 3); CHECK(r.pump() == 0);
  CHECK(!r.reconnect(idx).ok());                                 // device still gone
  port.restore();
  CHECK(r.reconnect(idx).ok());
  CHECK(t.status(id) == MappingStatus::Active);
  CHECK(r.pump() == 0 && got.empty());                           // reconnect delivers no old-generation actions
  port.injectInput({0x90, 60, 100}, 100 * kNsPerMs);
  CHECK(r.pump() == 1 && got.size() == 1);
  // fallback detection without callback
  port.closeInput(); r.checkPorts();
  CHECK(t.status(id) == MappingStatus::DeviceUnavailable);
}

TEST(midi_router_event_storm_bounded) {
  Rig g; g.r.setRateCeiling(1000000);
  for (int n = 0; n < 128; ++n) g.map(MidiMsgType::Note, n, ActionId::TapTempo);
  g.map(MidiMsgType::Note, 127, ActionId::TriggerCue);
  g.map(MidiMsgType::CC, 1, ActionId::LightingBlackout);
  size_t maxDepth = 0;
  for (int i = 0; i < 20000; ++i) {
    g.note(i % 127, 1 + (i / 127) % 127, i * 10000, i % 16 == 0 ? 0 : 0);
    maxDepth = std::max(maxDepth, g.r.stats().queueDepth);
    if (i == 15000) g.cc(1, 127, i * 10000 + 1);
  }
  RouterStats s = g.r.stats();
  CHECK(maxDepth <= 512 && s.queueDepth == 512);
  CHECK(s.loss > 0 && s.droppedOldest > 0);
  g.r.pump();
  bool blackout = false; for (auto& a : g.got) if (a.id == ActionId::LightingBlackout) blackout = true;
  CHECK(blackout);                                               // latch survived the storm
  CHECK(g.r.stats().queueDepth == 0);
  // overflow reported as alarm
  bool over = false; std::vector<MidiAlarm> al; g.r.setAlarmCallback([&](const MidiAlarm& a) { al.push_back(a); });
  for (int i = 0; i < 600; ++i) g.note(i % 100, 1 + i % 100, 1000000000LL + i * 100000);
  g.r.pump(); for (auto& a : al) if (a.kind == MidiAlarmKind::InputOverflow) over = true; CHECK(over);
}

TEST(midi_router_cc_storm_drops_stale_cc_first) {
  Rig g; g.r.setRateCeiling(1000000);
  for (int n = 0; n < 128; ++n) g.map(MidiMsgType::CC, n, ActionId::Brightness);
  g.map(MidiMsgType::Note, 60, ActionId::NextScene);
  for (int ch = 0; ch < 6; ++ch) for (int n = 0; n < 128; ++n) { g.cc(n, 5 + ch, 1000 + ch * 200 + n, ch); }   // 768 distinct controls
  g.note(60, 100, 5000000);
  RouterStats s = g.r.stats();
  CHECK(s.queueDepth == 512 && s.droppedStaleCc > 0 && s.droppedOldest == 0);
  g.r.pump();
  bool nextScene = false; for (auto& a : g.got) if (a.id == ActionId::NextScene) nextScene = true; CHECK(nextScene);   // trigger kept
}

TEST(midi_router_rate_ceiling_counter) {
  Rig g; for (int n = 0; n < 128; ++n) g.map(MidiMsgType::CC, n, ActionId::Speed);
  g.map(MidiMsgType::Note, 1, ActionId::EmergencyBlackout);
  for (int i = 0; i < 5000; ++i) g.cc(i % 128, i % 100, i * 100000);          // 5000 msgs in 0.5 s
  g.note(1, 127, 500 * kNsPerMs);                                              // critical bypasses ceiling
  RouterStats s = g.r.stats();
  CHECK(s.rateDropped >= 3900 && s.rateDropped <= 4000);
  g.r.pump(); bool bo = false; for (auto& a : g.got) if (a.id == ActionId::EmergencyBlackout) bo = true; CHECK(bo);
}

TEST(midi_feedback_route_graph_requires_explicit_enable) {
  MidiRouteGraph g; MidiRoute in; in.id = "in:pad"; in.kind = RouteKind::InputToAction; in.inPort = "pad"; in.enabled = true;
  CHECK(g.addRoute(in).ok() && g.isActive("in:pad") && !g.feedbackRisk("in:pad"));
  MidiRoute safe; safe.id = "out:synth"; safe.kind = RouteKind::ActionToOutput; safe.outPort = "synth"; safe.enabled = true;
  CHECK(g.addRoute(safe).ok() && g.isActive("out:synth"));
  MidiRoute fb; fb.id = "out:pad"; fb.kind = RouteKind::ActionToOutput; fb.outPort = "pad"; fb.enabled = true;   // same device as input
  CHECK(g.addRoute(fb).ok());
  CHECK(g.feedbackRisk("out:pad") && !g.isActive("out:pad"));
  CHECK(g.enable("out:pad", false).code == Err::PermissionDenied && !g.isActive("out:pad"));
  CHECK(g.enable("out:pad", true).ok() && g.isActive("out:pad"));
  MidiRoute thru; thru.id = "thru"; thru.kind = RouteKind::InputToOutput; thru.inPort = "pad"; thru.outPort = "synth"; thru.enabled = true;
  g.addRoute(thru); CHECK(!g.isActive("thru") && g.feedbackRisk("thru"));
  // device grouping: output "synth-out" declared the same device as input "synth-in" becomes risky once an input route exists
  MidiRoute i2; i2.id = "in:synth-in"; i2.kind = RouteKind::InputToAction; i2.inPort = "synth-in"; i2.enabled = true; g.addRoute(i2);
  MidiRoute o2; o2.id = "out:synth-out"; o2.kind = RouteKind::ActionToOutput; o2.outPort = "synth-out"; o2.enabled = true; g.addRoute(o2);
  CHECK(g.isActive("out:synth-out"));
  g.declareSameDevice("synth-in", "synth-out");
  CHECK(!g.isActive("out:synth-out") && g.feedbackRisk("out:synth-out"));        // existing route re-evaluated, disabled
  CHECK(g.suspend("out:pad", "test") && !g.isActive("out:pad"));
  CHECK(g.enable("out:pad", true).ok() && g.isActive("out:pad"));
  CHECK(!g.addRoute(in).ok());
}

TEST(midi_feedback_loop_trips_protection) {
  Rig g; g.map(MidiMsgType::Note, 60, ActionId::NextScene);
  std::vector<MidiAlarm> al; g.r.setAlarmCallback([&](const MidiAlarm& a) { al.push_back(a); });
  // device echoes the same trigger 40x within 0.4 s (e.g. a looped controller)
  for (int i = 0; i < 40; ++i) g.note(60, 100, i * 10 * kNsPerMs);
  CHECK(g.r.alarmLatched());
  CHECK(!g.g.isActive("in:dev"));                                // offending route suspended
  MidiRoute rt; CHECK(g.g.get("in:dev", rt) && rt.suspended && !rt.suspendReason.empty());
  RouterStats s = g.r.stats();
  CHECK(s.echoTrips == 1 && s.suspendedDropped >= 15);
  g.r.pump();
  CHECK(g.got.empty());                                          // pending events of the looping route discarded
  bool echo = false; for (auto& a : al) if (a.kind == MidiAlarmKind::EchoLoop) echo = true; CHECK(echo);
  g.note(60, 100, 2 * kNsPerSec); g.r.pump(); CHECK(g.got.empty());               // stays suspended
  g.r.clearAlarm(); CHECK(!g.r.alarmLatched() && !g.g.isActive("in:dev"));       // alarm reset does not silently resume
  CHECK(g.g.enable("in:dev", false).ok());
  g.note(60, 100, 5 * kNsPerSec); g.r.pump(); CHECK(g.got.size() == 1);
}

TEST(midi_legit_repeats_and_clock_not_suppressed) {
  Rig g; g.map(MidiMsgType::Note, 60, ActionId::NextScene);
  // 10 identical per second for 5 s (fast but legitimate) must not trip
  for (int i = 0; i < 50; ++i) g.note(60, 100, i * 100 * kNsPerMs);
  CHECK(!g.r.alarmLatched());
  // MIDI clock 24 ppqn @ 300 BPM = 120/s, plus a 10000-pulse flood: never counted, dropped or queued
  uint8_t clk = 0xF8;
  for (int i = 0; i < 10000; ++i) g.r.onBytes(g.p, 10 * kNsPerSec + i * 100000, &clk, 1);
  RouterStats s = g.r.stats();
  CHECK(!g.r.alarmLatched() && s.realtime == 10000 && s.rateDropped == 0 && g.g.isActive("in:dev"));
  g.note(60, 100, 20 * kNsPerSec); g.r.pump(); CHECK(g.got.size() >= 1);
}

TEST(midi_origin_blocks_action_midi_recursion_via_loopback) {
  // Closed loop: MIDI in -> action -> (feedback) MIDI out -> loopback -> MIDI in. Origin tag stops it at the first hop.
  Rig g; g.map(MidiMsgType::Note, 60, ActionId::NextScene);
  int emitted = 0;
  g.r.setActionSink([&](const Action& a) { if (actionMayEmitMidi(a)) ++emitted; });
  g.note(60, 100, 0); g.r.pump();
  CHECK(emitted == 0);
}

int main() { return ss_test::runAll(); }
