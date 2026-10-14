#include "test.h"
#include "midi/output.h"
#include "midi/mock_port.h"
#include <chrono>
#include <thread>
using namespace ss;

namespace {
struct Rig {
  FakeClock clk; MockMidiPort port{&clk};
  MidiOutConfig cfg;
  std::unique_ptr<MidiOutputScheduler> s;
  explicit Rig(MidiOutConfig c = MidiOutConfig()) : cfg(c) { port.openOutput("out"); s.reset(new MidiOutputScheduler(clk, port, cfg)); }
  void advanceTo(Ns t) { if (t > clk.t) clk.advance(t - clk.t); }
};
}

TEST(midi_out_notes_cc_order_and_deadlines) {
  Rig r;
  r.clk.advance(1000);
  CHECK(r.s->noteOn(0, 60, 100, 5 * kNsPerMs).ok());
  CHECK(r.s->cc(1, 7, 64, 2 * kNsPerMs).ok());
  CHECK(r.s->noteOff(0, 60, 9 * kNsPerMs).ok());
  CHECK(r.s->noteOn(0, 61, 90, 5 * kNsPerMs).ok());              // same deadline: FIFO
  r.s->step(); CHECK(r.port.sentCount() == 0);
  Ns next = r.s->step(); CHECK(next > 0);
  r.advanceTo(2 * kNsPerMs + 1000); r.s->step();
  CHECK(r.port.sentCount() == 1 && r.port.sent()[0].bytes == std::vector<uint8_t>({0xB1, 7, 64}));
  r.advanceTo(20 * kNsPerMs); r.s->step();
  auto sent = r.port.sent();
  CHECK(sent.size() == 4);
  CHECK(sent[1].bytes[1] == 60 && sent[2].bytes[1] == 61 && sent[3].bytes[0] == 0x80);
  CHECK(!r.s->noteOn(16, 60, 1).ok() && !r.s->cc(0, 200, 1).ok());
  MidiOutMessage bad; bad.bytes = {0x90, 60}; CHECK(!r.s->submit(bad).ok());
  bad.bytes = {0x90, 200, 1}; CHECK(!r.s->submit(bad).ok());
  CHECK(r.s->stats().rejected >= 2);
}

TEST(midi_out_clock_24ppqn_spacing_exact) {
  Rig r; r.s->setFixedTempo(120); r.s->setClockEnabled(true);
  Ns period = (Ns)(60e9 / (120 * 24.0));
  CHECK(period == 20833333);
  while (r.clk.t <= 2 * kNsPerSec) { Ns nx = r.s->step(); CHECK(nx != MidiOutputScheduler::kNoDeadline); r.advanceTo(nx); }
  auto sent = r.port.sent();
  CHECK(sent.size() >= 96 && sent.size() <= 98);                 // 2 s * 48 pulses/s (+ first at t=0)
  for (auto& m : sent) CHECK(m.bytes.size() == 1 && m.bytes[0] == 0xF8);
  for (size_t i = 1; i < sent.size(); ++i) { Ns d = sent[i].t - sent[i - 1].t; CHECK(d == period || d == period + 1 || d == period - 1); }
  MidiJitter j = r.s->jitter();
  CHECK(j.count == sent.size() - 1 && j.p95ErrNs <= 1 && j.medianErrNs <= 1 && j.missedPulses == 0);
  r.s->setClockEnabled(false); size_t n = r.port.sentCount(); r.advanceTo(r.clk.t + kNsPerSec); r.s->step();
  CHECK(r.port.sentCount() == n);
}

TEST(midi_out_clock_jitter_1ms_ticks_within_budget) {
  Rig r; double bpm = 120; r.s->setTempoSource([&] { return bpm; }); r.s->setClockEnabled(true);
  Ns end = 10 * kNsPerSec;                                       // quantised 1 ms worker wakeups (worst-case scheduler)
  while (r.clk.t < end) { r.s->step(); r.clk.advance(kNsPerMs); }
  MidiJitter j = r.s->jitter();
  CHECK(j.count > 450);
  CHECK(j.medianErrNs <= 1.0 * kNsPerMs && j.p95ErrNs <= 3.0 * kNsPerMs);
  CHECK(j.missedPulses == 0);
  CHECK(r.s->stats().clockPulses >= 479 && r.s->stats().clockPulses <= 482);   // no drift: deadlines accumulate
  // tempo change takes effect from the next pulse
  bpm = 60; size_t before = r.port.sentCount(); Ns t0 = r.clk.t;
  while (r.clk.t < t0 + kNsPerSec) { r.s->step(); r.clk.advance(kNsPerMs); }
  size_t d = r.port.sentCount() - before; CHECK(d >= 23 && d <= 25);
}

TEST(midi_out_clock_stall_skips_not_bursts) {
  Rig r; r.s->setFixedTempo(120); r.s->setClockEnabled(true);
  r.s->step(); r.advanceTo(21 * kNsPerMs); r.s->step();
  size_t n = r.port.sentCount();
  r.advanceTo(r.clk.t + 500 * kNsPerMs); r.s->step();            // worker stalled 0.5 s
  CHECK(r.port.sentCount() == n + 1);
  CHECK(r.s->stats().clockSkipped >= 20 && r.s->jitter().missedPulses >= 20);
  r.s->setTempoSource([] { return 0.0; }); r.advanceTo(r.clk.t + kNsPerSec); r.s->step();   // unknown tempo: no pulses
  CHECK(r.port.sentCount() == n + 1);
}

TEST(midi_out_transport_separately_gated_clock_opt_in) {
  Rig r;
  CHECK(r.s->sendStart().code == Err::PermissionDenied && r.s->sendStop().code == Err::PermissionDenied && r.s->sendContinue().code == Err::PermissionDenied);
  r.s->step(); CHECK(r.port.sentCount() == 0 && !r.s->clockEnabled());   // clock off by default
  MidiOutConfig c = r.s->config(); c.allowStart = true; c.allowStop = true; r.s->setConfig(c);
  CHECK(r.s->sendStart().ok() && r.s->sendStop().ok() && r.s->sendContinue().code == Err::PermissionDenied);
  r.s->step(); auto s = r.port.sent();
  CHECK(s.size() == 2 && s[0].bytes[0] == 0xFA && s[1].bytes[0] == 0xFC);
  MidiOutMessage raw; raw.bytes = {0xF8}; CHECK(!r.s->submit(raw).ok());   // clock only via generator
  raw.origin = MidiOrigin::MidiInput; raw.bytes = {0x90, 1, 1}; CHECK(r.s->submit(raw).code == Err::PermissionDenied);
}

TEST(midi_out_owned_note_cleanup_only_ours) {
  Rig r;
  r.s->noteOn(0, 60, 100); r.s->noteOn(2, 64, 100); r.s->noteOn(0, 67, 100); r.s->noteOff(0, 67);
  r.s->step(); CHECK(r.s->ownedNoteCount() == 2);
  r.port.clearSent();
  CHECK(r.s->releaseOwnedNotes() == 2);
  r.s->step();
  auto s = r.port.sent();
  CHECK(s.size() == 2 && r.s->ownedNoteCount() == 0);
  for (auto& m : s) CHECK(m.bytes.size() == 3 && (m.bytes[0] & 0xF0) == 0x80 && (m.bytes[1] == 60 || m.bytes[1] == 64));
  // stop() releases remaining owned notes directly; never sends controller 123 (All Notes Off)
  r.s->noteOn(5, 40, 100); r.s->step(); r.port.clearSent();
  r.s->stop();
  s = r.port.sent();
  CHECK(s.size() == 1 && s[0].bytes == std::vector<uint8_t>({0x85, 40, 0}));
  r.s->stop(); r.s->stop(); CHECK(r.port.sentCount() == 1);       // idempotent
  for (auto& m : r.port.sent()) CHECK(!(m.bytes[0] == 0xB0 && m.bytes[1] == 123));
}

TEST(midi_out_velocity_zero_noteon_clears_ownership) {
  Rig r; MidiOutMessage m; m.bytes = {0x90, 50, 100}; r.s->submit(m); r.s->step(); CHECK(r.s->ownedNoteCount() == 1);
  m.bytes = {0x90, 50, 0}; r.s->submit(m); r.s->step(); CHECK(r.s->ownedNoteCount() == 0);
}

TEST(midi_out_sysex_disabled_and_limit) {
  Rig r;
  std::vector<uint8_t> sx(1024, 0x11); sx.front() = 0xF0; sx.back() = 0xF7;
  CHECK(r.s->sysex(sx).code == Err::PermissionDenied);
  MidiOutConfig c = r.s->config(); c.sysexEnabled = true; r.s->setConfig(c);
  CHECK(r.s->sysex(sx).ok());                                    // exactly 1024 bytes
  std::vector<uint8_t> big(1025, 0x11); big.front() = 0xF0; big.back() = 0xF7;
  CHECK(r.s->sysex(big).code == Err::InvalidArgument);
  std::vector<uint8_t> open = {0xF0, 1, 2}; CHECK(!r.s->sysex(open).ok());
  std::vector<uint8_t> hi = {0xF0, 1, 0x90, 0xF7}; CHECK(!r.s->sysex(hi).ok());
  r.s->step(); CHECK(r.port.sentCount() == 1 && r.port.sent()[0].bytes.size() == 1024 && r.s->stats().sysex == 1);
}

TEST(midi_out_stale_deadline_coalescing) {
  Rig r;
  for (int i = 0; i < 5; ++i) r.s->cc(0, 7, 10 + i, (10 + i * 5) * kNsPerMs);
  r.s->cc(0, 8, 1, 11 * kNsPerMs);
  r.s->noteOn(0, 60, 100, 12 * kNsPerMs); r.s->noteOn(0, 60, 101, 13 * kNsPerMs);
  r.advanceTo(500 * kNsPerMs); r.s->step();                      // everything is >20 ms stale
  auto s = r.port.sent();
  int c7 = 0, c8 = 0, n = 0; uint8_t last7 = 0;
  for (auto& m : s) { if (m.bytes[0] == 0xB0 && m.bytes[1] == 7) { ++c7; last7 = m.bytes[2]; } if (m.bytes[0] == 0xB0 && m.bytes[1] == 8) ++c8; if (m.bytes[0] == 0x90) ++n; }
  CHECK(c7 == 1 && last7 == 14 && c8 == 1 && n == 2);          // CCs coalesced to latest; notes never dropped
  CHECK(r.s->stats().coalesced == 4);
  // fresh CCs are not coalesced
  r.port.clearSent();
  r.s->cc(0, 7, 1, r.clk.t); r.s->cc(0, 7, 2, r.clk.t); r.s->step(); CHECK(r.port.sentCount() == 2);
}

TEST(midi_out_rate_ceiling_exempts_clock_and_noteoff) {
  Rig r; r.s->setFixedTempo(300); r.s->setClockEnabled(true);
  for (int i = 0; i < 400; ++i) r.s->cc(i % 16, i % 128, 1);
  MidiOutConfig c = r.s->config(); c.maxMsgsPerSec = 100; c.queueCap = 1000; r.s->setConfig(c);
  for (int i = 0; i < 300; ++i) r.s->cc(i % 16, i % 128, 2);
  r.s->step();
  MidiOutStats st = r.s->stats();
  CHECK(st.rateDropped > 0 && st.sent <= 100 + 1);
  CHECK(st.clockPulses == 1);
  for (int i = 0; i < 200; ++i) { r.s->noteOff(0, i % 128); }
  r.s->step(); CHECK(r.s->stats().sent > 150);                    // note-offs always pass
}

TEST(midi_out_queue_bounded_and_port_loss) {
  Rig r;
  int ok = 0, full = 0;
  for (int i = 0; i < 700; ++i) { Status s = r.s->cc(0, i % 128, 1, kNsPerSec); if (s.ok()) ++ok; else if (s.code == Err::ResourceExhausted) ++full; }
  CHECK(ok == 512 && full == 188 && r.s->stats().pending == 512);
  uint32_t g0 = r.s->generation();
  r.s->noteOn(0, 1, 1); r.s->step();
  r.s->notePortLost();
  CHECK(r.s->generation() != g0 && r.s->stats().pending == 0 && r.s->ownedNoteCount() == 0);
  CHECK(r.s->cc(0, 1, 1).code == Err::DeviceLost);
  r.s->notePortRestored();
  r.clk.advance(2 * kNsPerSec); r.port.clearSent(); r.s->step(); CHECK(r.port.sentCount() == 0);   // no stale queued messages
  MidiOutMessage m; m.bytes = {0xB0, 1, 1}; m.generation = g0; CHECK(r.s->submit(m).code == Err::Cancelled);
  m.generation = r.s->generation(); CHECK(r.s->submit(m).ok());
  m.armToken = 9; CHECK(r.s->submit(m).code == Err::Cancelled); r.s->arm(9); CHECK(r.s->submit(m).ok());
  // send failure DeviceLost from the port is detected
  r.port.simulateLoss(false); r.s->step(); r.s->step();
  CHECK(r.s->stats().sendFailures >= 1);
  CHECK(r.s->cc(0, 1, 1).code == Err::DeviceLost);
}

TEST(midi_out_loopback_and_drop_simulation) {
  Rig r; r.port.setLoopback(true);
  FakeClock rc; MappingTable t; MidiRouteGraph g; MidiRouter rt(t, g, rc);
  MidiPortInfo info; info.stableId = "dev"; info.name = "Dev"; r.port.addPort(info);
  MidiMapping m; m.deviceId = "dev"; m.type = MidiMsgType::Note; m.number = 60; m.action = ActionId::NextScene; t.add(m);
  std::vector<Action> got; rt.setActionSink([&](const Action& a) { got.push_back(a); });
  CHECK(rt.attach(r.port, info).ok());
  r.s->noteOn(0, 60, 100); r.s->step();
  CHECK(rt.pump() == 1 && got.size() == 1 && got[0].source == ActionSource::Midi);
  // origin-tag guard: a MIDI-derived action may not be fed back out
  if (actionMayEmitMidi(got[0])) r.s->noteOn(0, 60, 100);
  r.s->step(); CHECK(rt.pump() == 0 && r.port.sentCount() == 1);
  // wire drops
  r.port.clearSent(); r.port.dropNext(2);
  for (int i = 0; i < 5; ++i) r.s->cc(0, i, 1);
  r.s->step(); CHECK(r.port.sentCount() == 3 && r.port.dropped() == 2);
  r.port.setDropEvery(2); r.port.clearSent();
  for (int i = 0; i < 6; ++i) r.s->cc(0, i, 1);
  r.s->step(); CHECK(r.port.sentCount() == 3);
}

TEST(midi_out_worker_thread_start_stop_idempotent) {
  MockMidiPort port(&systemClock()); port.openOutput("o");
  MidiOutputScheduler s(systemClock(), port);
  s.stop(); s.stop();                                            // before start
  s.start(); s.start(); CHECK(s.running());
  s.noteOn(0, 60, 100);
  s.noteOn(0, 61, 100, systemClock().nowNs() + 30 * kNsPerMs);
  for (int i = 0; i < 30 && port.sentCount() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(port.sentCount() == 2);
  s.stop(); s.stop(); CHECK(!s.running());
  CHECK(port.sentCount() == 4);                                  // two owned note-offs released at stop
  s.start(); s.noteOn(1, 1, 1); for (int i = 0; i < 20 && port.sentCount() < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(port.sentCount() >= 5);
}                                                                // destructor stops again

TEST(midi_out_thread_clock_real_time_smoke) {
  MockMidiPort port(&systemClock()); port.openOutput("o");
  MidiOutputScheduler s(systemClock(), port);
  s.setFixedTempo(240); s.setClockEnabled(true); s.start();      // 96 pulses/s
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  s.stop();
  size_t n = port.sentCount();
  CHECK(n >= 5 && n <= 25);
  for (auto& m : port.sent()) CHECK(m.bytes[0] == 0xF8);
}

int main() { return ss_test::runAll(); }
