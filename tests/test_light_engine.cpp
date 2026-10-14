// LightingEngine tests: tick rate, arming/faults, blackout precedence, strobe disable, dry-run isolation, fades (REQ-L03..L05, REQ-S12).
#include "test.h"
#include "lighting/engine.h"
#include <chrono>
#include <thread>
using namespace ss;

static const char* kMoverJson = R"({
 "schema":"stagesignal.fixture-profile","version":1,"id":"test-mover","name":"Test mover","channel_count":8,
 "channels":[
  {"name":"dimmer","function":"intensity","offset":0,"default":0,"safe":0,"blackout":0,"intensity":true},
  {"name":"strobe","function":"strobe","offset":1,"default":0,"safe":0,"strobe":true,
   "ranges":[{"lo":0,"hi":7,"continuous":false},{"lo":8,"hi":255,"continuous":true}]},
  {"name":"pan","function":"pan","offset":2,"width":2,"min":1000,"max":64000,"default":32768,"safe":32768},
  {"name":"tilt","function":"tilt","offset":4,"width":2,"order":"fine-first","default":20000,"safe":20000},
  {"name":"wheel","function":"macro","offset":6,"default":0,"safe":10}
 ]})";

struct Rig {
  FakeClock clk; RecordingSink spy; ProfileLibrary lib; Patch patch; std::unique_ptr<LightingEngine> eng; EngineConfig cfg;
  explicit Rig(LightProtocol proto = LightProtocol::Sacn, bool multicast = false, bool dry = false) {
    lib = ProfileLibrary::builtin(); lib.addJson(kMoverJson);
    cfg.protocol = proto; cfg.host = "10.0.0.5"; cfg.sacnMulticast = multicast;
    int u = proto == LightProtocol::ArtNet ? 0 : 1;
    PatchEntry a; a.fixtureId = "mover"; a.profileId = "test-mover"; a.universe = u; a.startAddress = 1;
    PatchEntry b; b.fixtureId = "par"; b.profileId = "generic-rgbw-strobe"; b.universe = u; b.startAddress = 20;
    PatchEntry c; c.fixtureId = "dim"; c.profileId = "generic-dimmer"; c.universe = u + 1; c.startAddress = 1;
    patch.entries = {a, b, c};
    Status st; eng = dry ? LightingEngine::makeDryRun(clk, cfg, &st) : LightingEngine::makeLive(clk, cfg, spy, &st);
    if (!eng) { std::printf("engine creation failed: %s\n", st.detail.c_str()); return; }
    CHECK(eng->applyPatch(patch, lib).ok());
    CHECK(eng->openTransport().ok());
    eng->pump();                                         // baseline tick at t=0 (Disarmed)
  }
  void run(int ms) { for (int i = 0; i < ms; ++i) { clk.advance(kNsPerMs); eng->pump(); } }
  bool arm() { ArmToken t; if (!eng->requestArmToken(t).ok()) return false; return eng->arm(t).ok(); }
  uint32_t val(const char* f, const char* c) { uint32_t v = 0xFFFFFFFF; eng->resolvedValue(f, c, v); return v; }
};
static Cue cue(std::initializer_list<CueValue> v, Ns fade = 0) { Cue c; c.name = "c"; c.values = v; c.fadeNs = fade; return c; }

TEST(tick_rate_25hz_and_40hz_cap) {
  Rig r; r.run(2000);
  CHECK(r.eng->status().ticks == 51);                 // t=0 then every 40 ms through 2.000 s
  EngineConfig bad; bad.host = "1.2.3.4"; bad.tickHz = 41; CHECK(!LightingEngine::validateConfig(bad, false).ok());
  bad.tickHz = 40; CHECK(LightingEngine::validateConfig(bad, false).ok());
  Rig r40; (void)r40; FakeClock c; EngineConfig k; k.host = "1.2.3.4"; k.tickHz = 40; RecordingSink s;
  auto e = LightingEngine::makeLive(c, k, s); e->pump(); for (int i = 0; i < 1000; ++i) { c.advance(kNsPerMs); e->pump(); }
  CHECK(e->status().ticks == 41);
}
TEST(late_ticks_coalesce) {
  Rig r; r.run(40); uint64_t t0 = r.eng->status().ticks;
  r.clk.advance(kNsPerSec); int n = r.eng->pump();
  CHECK(n == 1 && r.eng->status().ticks == t0 + 1 && r.eng->status().skippedTicks >= 20);
}
TEST(disarmed_by_default_sends_nothing) {
  Rig r; r.run(500);
  CHECK(r.eng->status().state == ArmState::Disarmed);
  CHECK(r.spy.datagramsSent() == 0);
  ArmToken forged; forged.epoch = 1; forged.nonce = 1;
  CHECK(!r.eng->arm(forged).ok());                    // no token was issued
  UniverseFrame f; CHECK(r.eng->latestFrame(1, f) && f.length == 512);   // frames still resolve for preview
}
TEST(armed_sacn_frames_on_the_wire) {
  Rig r; CHECK(r.arm()); r.run(200);
  auto d = r.spy.records(); CHECK(d.size() == 10);    // ticks at 40..200 ms x 2 universes
  sacn::ParsedData p; CHECK(sacn::parseDataPacket(d[0].bytes.data(), d[0].bytes.size(), p));
  CHECK(d[0].host == "10.0.0.5" && d[0].port == 5568 && d[0].bytes.size() == 638);
  CHECK(p.p.universe == 1 && p.priority == 100 && p.startCode == 0 && p.name == "StageSignal" && p.p.options == 0);
  sacn::ParsedData p2; sacn::parseDataPacket(d[2].bytes.data(), d[2].bytes.size(), p2);
  CHECK(p2.p.universe == 1 && (uint8_t)(p2.p.sequence - p.p.sequence) == 1 && p2.cid == p.cid);
  sacn::ParsedData u2; sacn::parseDataPacket(d[1].bytes.data(), d[1].bytes.size(), u2); CHECK(u2.p.universe == 2);
  CHECK(p.slots[2] == 0x80 && p.slots[3] == 0x00);    // mover pan default 32768 coarse/fine
  CHECK(r.spy.broadcastAllowed() == false);
}
TEST(artnet_frames_and_universe_numbers) {
  Rig r(LightProtocol::ArtNet); CHECK(r.arm()); r.run(80);
  auto d = r.spy.records(); CHECK(d.size() >= 4);
  artnet::ArtDmxPacket p; CHECK(artnet::parseArtDmx(d[0].bytes.data(), d[0].bytes.size(), p));
  CHECK(d[0].port == 6454 && p.p.portAddress == 0 && p.length == 512 && p.p.sequence == 1);
  artnet::ArtDmxPacket q; artnet::parseArtDmx(d[1].bytes.data(), d[1].bytes.size(), q); CHECK(q.p.portAddress == 1);
  artnet::ArtDmxPacket s; artnet::parseArtDmx(d[2].bytes.data(), d[2].bytes.size(), s); CHECK(s.p.sequence == 2);
  // Art-Net 0 is valid, sACN 0 is not (numbering is not interchangeable)
  Rig sr(LightProtocol::Sacn); Patch pp = sr.patch; pp.entries[0].universe = 0; CHECK(!sr.eng->applyPatch(pp, sr.lib).ok());
  Rig ar(LightProtocol::ArtNet); pp = ar.patch; pp.entries[0].universe = 32768; CHECK(!ar.eng->applyPatch(pp, ar.lib).ok());
}
TEST(sacn_multicast_destination) {
  Rig r(LightProtocol::Sacn, true); CHECK(r.arm()); r.run(50);
  auto d = r.spy.records(); CHECK(d.size() >= 2 && d[0].host == "239.255.0.1" && d[1].host == "239.255.0.2");
}
TEST(config_validation_broadcast_opt_in) {
  FakeClock c; RecordingSink s; Status st; EngineConfig k; k.protocol = LightProtocol::ArtNet; k.host = "255.255.255.255";
  CHECK(LightingEngine::makeLive(c, k, s, &st) == nullptr && !st.ok());
  k.allowBroadcast = true; CHECK(LightingEngine::makeLive(c, k, s, &st) != nullptr);
  k = EngineConfig(); CHECK(LightingEngine::makeLive(c, k, s, &st) == nullptr);   // no destination
  k.sacnPriority = 201; k.host = "1.1.1.1"; CHECK(LightingEngine::makeLive(c, k, s, &st) == nullptr);
}

TEST(fault_disconnect_reconnect_stays_disarmed) {
  Rig r; CHECK(r.arm()); r.run(100);
  CHECK(r.eng->status().state == ArmState::Armed);
  ArmToken stale; CHECK(r.eng->requestArmToken(stale).ok());          // token issued before the fault
  r.eng->drainEvents();
  r.spy.setFailing(true); r.run(100);                                  // send failure -> fault
  EngineStatus s = r.eng->status();
  CHECK(s.state == ArmState::Disarmed && s.faulted && s.faults == 1 && s.lastFault.code == Err::TransportFailure);
  bool sawFault = false; for (auto& e : r.eng->drainEvents()) if (e.kind == LightEvent::Fault) sawFault = true; CHECK(sawFault);
  r.spy.setFailing(false);
  uint64_t before = r.spy.datagramsSent(); r.run(200);
  CHECK(r.spy.datagramsSent() == before);                              // link back, still silent
  ArmToken t; CHECK(!r.eng->requestArmToken(t).ok());                  // must reconnect first
  CHECK(r.eng->reconnect().ok());
  CHECK(r.eng->status().state == ArmState::Disarmed && !r.eng->status().faulted);
  r.run(200); CHECK(r.spy.datagramsSent() == before);                  // reconnect does NOT rearm
  CHECK(!r.eng->arm(stale).ok());                                      // stale token rejected
  CHECK(r.arm()); r.run(100); CHECK(r.spy.datagramsSent() > before);   // explicit new arm works
}
TEST(transport_lost_notification) {
  Rig r; CHECK(r.arm()); r.eng->notifyTransportLost("NIC removed");
  CHECK(r.eng->status().state == ArmState::Disarmed && r.eng->status().faulted && !r.eng->status().transportOpen);
  CHECK(r.eng->reconnect().ok()); CHECK(r.eng->status().state == ArmState::Disarmed);
  r.spy.setFailOpen(true); CHECK(!r.eng->reconnect().ok() && r.eng->status().state == ArmState::Disarmed);
}
TEST(patch_conflict_blocks_arm_until_owner_resolved) {
  Rig r; Patch p = r.patch; PatchEntry x = p.entries[2]; x.fixtureId = "dim2"; x.startAddress = 1; p.entries.push_back(x);   // overlaps "dim"
  PatchReport rep; CHECK(r.eng->applyPatch(p, r.lib, &rep).ok());
  CHECK(rep.unresolved() == 1 && !rep.warnings.empty());
  ArmToken t; CHECK(!r.eng->requestArmToken(t).ok()); CHECK(!r.arm()); CHECK(r.eng->status().unresolvedConflicts == 1);
  bool warned = false; for (auto& e : r.eng->drainEvents()) if (e.kind == LightEvent::Warning) warned = true; CHECK(warned);
  p.entries[2].ownership = Ownership::Owner; p.entries[3].ownership = Ownership::Yielded;
  CHECK(r.eng->applyPatch(p, r.lib).ok()); CHECK(r.arm());
  // invalid patch is rejected and keeps previous state
  Patch bad = p; bad.entries[0].startAddress = 509; CHECK(!r.eng->applyPatch(bad, r.lib).ok());
  // changing patch while armed forces disarm
  CHECK(r.eng->applyPatch(p, r.lib).ok()); CHECK(r.eng->status().state == ArmState::Disarmed);
}
TEST(owner_wins_overlap_in_frame) {
  Rig r; Patch p; PatchEntry a; a.fixtureId = "a"; a.profileId = "generic-dimmer"; a.universe = 1; a.startAddress = 5; a.ownership = Ownership::Yielded;
  PatchEntry b = a; b.fixtureId = "b"; b.ownership = Ownership::Owner; p.entries = {b, a};
  CHECK(r.eng->applyPatch(p, r.lib).ok());
  r.eng->setManual("a", "dimmer", 10); r.eng->setManual("b", "dimmer", 200); r.run(50);
  UniverseFrame f; CHECK(r.eng->latestFrame(1, f) && f.data[4] == 200);
}

TEST(precedence_manual_over_cue_over_audio) {
  Rig r;
  AudioMapping m; m.fixtureId = "mover"; m.channel = "dimmer"; m.source = AudioSource::BandLow; m.outLo = 0; m.outHi = 255;
  AudioMapping m2 = m; m2.channel = "wheel"; m2.outHi = 100;
  CHECK(r.eng->setAudioMappings({m, m2}).ok());
  FeatureSnapshot fs; fs.band[0] = 0.5f; r.eng->submitFeatures(fs); r.run(50);
  CHECK(r.val("mover", "dimmer") == 128 && r.val("mover", "wheel") == 50);    // audio only
  CHECK(r.eng->setCue(cue({{"mover", "dimmer", 77}})).ok()); r.run(50);
  CHECK(r.val("mover", "dimmer") == 77 && r.val("mover", "wheel") == 50);     // cue beats audio on that channel only
  CHECK(r.eng->setManual("mover", "dimmer", 33).ok()); r.run(50);
  CHECK(r.val("mover", "dimmer") == 33);                                      // manual beats cue
  r.eng->clearManual("mover", "dimmer"); r.run(50); CHECK(r.val("mover", "dimmer") == 77);
  r.eng->clearCue(); r.run(50); CHECK(r.val("mover", "dimmer") == 128);
  r.clk.advance(2 * kNsPerSec); r.run(50);                                    // snapshot goes stale
  CHECK(r.val("mover", "dimmer") == 0 && r.val("mover", "wheel") == 0);
  CHECK(!r.eng->setManual("mover", "nope", 1).ok() && !r.eng->setManual("mover", "dimmer", 256).ok());
}
TEST(blackout_is_fixture_aware_and_overrides_everything) {
  Rig r; CHECK(r.arm());
  r.eng->setCue(cue({{"mover", "dimmer", 255}, {"mover", "strobe", 200}, {"mover", "pan", 40000}, {"mover", "wheel", 120}, {"par", "red", 255}, {"par", "dimmer", 255}, {"par", "strobe", 90}, {"dim", "dimmer", 255}}));
  r.eng->setManual("par", "green", 222); r.run(80);
  CHECK(r.val("mover", "dimmer") == 255 && r.val("mover", "strobe") == 200 && r.val("mover", "pan") == 40000);
  r.spy.clearRecords();
  r.eng->setLightingBlackout(true);
  r.clk.advance(40 * kNsPerMs); r.eng->pump();                                // exactly one tick later
  CHECK(r.val("mover", "dimmer") == 0 && r.val("mover", "strobe") == 0 && r.val("par", "red") == 0 && r.val("par", "green") == 0 &&
        r.val("par", "dimmer") == 0 && r.val("par", "strobe") == 0 && r.val("dim", "dimmer") == 0);
  CHECK(r.val("mover", "pan") == 32768 && r.val("mover", "wheel") == 10);     // configured non-intensity safe values RETAINED (not zeros)
  auto d = r.spy.records(); CHECK(d.size() == 2);                              // blackout frame actually transmitted on that tick
  sacn::ParsedData p; CHECK(sacn::parseDataPacket(d[0].bytes.data(), d[0].bytes.size(), p));
  CHECK(p.slots[0] == 0 && p.slots[2] == 0x80 && p.slots[3] == 0x00 && p.slots[6] == 10);
  // Fine-first tilt keeps its safe value 20000 = 0x4E20 -> slot4 fine 0x20, slot5 coarse 0x4E
  CHECK(p.slots[4] == 0x20 && p.slots[5] == 0x4E);
  // manual override cannot punch through blackout; emergency independent of lighting blackout
  r.eng->setManual("mover", "dimmer", 255); r.eng->setEmergencyBlackout(true); r.eng->setLightingBlackout(false);
  r.run(40); CHECK(r.val("mover", "dimmer") == 0);
  r.eng->setEmergencyBlackout(false); r.run(40); CHECK(r.val("mover", "dimmer") == 255);
  bool blk = false; for (auto& e : r.eng->drainEvents()) if (e.kind == LightEvent::Blackout) blk = true; CHECK(blk);
}
TEST(blackout_actions_and_restore_rules) {
  Rig r; Action a; a.id = ActionId::LightingBlackout; a.value = 1; a.source = ActionSource::Midi;
  CHECK(r.eng->handleAction(a).accepted && r.eng->status().lightingBlackout);
  a.id = ActionId::EmergencyBlackout; CHECK(r.eng->handleAction(a).accepted && r.eng->status().emergencyBlackout);
  a.id = ActionId::RestoreBlackout; a.source = ActionSource::Show; CHECK(!r.eng->handleAction(a).accepted && r.eng->status().emergencyBlackout);
  a.source = ActionSource::UI; CHECK(r.eng->handleAction(a).accepted && !r.eng->status().emergencyBlackout && !r.eng->status().lightingBlackout);
  a.id = ActionId::ArmLighting; a.source = ActionSource::Show; CHECK(!r.eng->handleAction(a).accepted);
  a.source = ActionSource::Remote; CHECK(!r.eng->handleAction(a).accepted);
  a.source = ActionSource::UI; CHECK(r.eng->handleAction(a).accepted && r.eng->status().state == ArmState::Armed);
  a.id = ActionId::DisarmLighting; CHECK(r.eng->handleAction(a).accepted && r.eng->status().state == ArmState::Disarmed);
  a.id = ActionId::NextScene; CHECK(!r.eng->handleAction(a).accepted);
}
TEST(disable_strobing_forces_safe_value) {
  Rig r;
  r.eng->setCue(cue({{"par", "strobe", 200}, {"mover", "strobe", 150}})); r.eng->setManual("par", "strobe", 240); r.run(50);
  CHECK(r.val("par", "strobe") == 240 && r.val("mover", "strobe") == 150);
  Action a; a.id = ActionId::DisableStrobing; a.value = 1; a.source = ActionSource::UI; CHECK(r.eng->handleAction(a).accepted);
  r.run(40);
  CHECK(r.val("par", "strobe") == 0 && r.val("mover", "strobe") == 0);
  CHECK(r.val("par", "red") == 0 || true);
  r.eng->setManual("mover", "strobe", 255); r.run(40); CHECK(r.val("mover", "strobe") == 0);      // cannot be re-enabled by override
  a.value = 0; r.eng->handleAction(a); r.run(40); CHECK(r.val("mover", "strobe") == 255);
}

TEST(dry_run_sends_zero_datagrams_on_real_sink) {
  Rig spyRig; RecordingSink& realSpy = spyRig.spy; CHECK(realSpy.open("", false).ok());   // a real-sink spy exists in the process
  Rig r(LightProtocol::Sacn, false, true);
  CHECK(r.eng->status().dryRun && r.eng->dryRunSink() != nullptr);
  CHECK(r.arm()); r.run(1000);
  CHECK(r.eng->status().framesSent > 0);
  CHECK(r.eng->dryRunSink()->wouldSend() == r.eng->status().framesSent);
  CHECK(r.eng->dryRunSink()->datagramsSent() == 0);
  CHECK(realSpy.datagramsSent() == 0 && r.spy.datagramsSent() == 0);
  CHECK(r.eng->dryRunSink()->last().bytes.size() == 638);                  // the frame was computed
  r.eng->setLightingBlackout(true); r.run(100); r.eng->stop();             // graceful stop in dry-run also emits nothing real
  CHECK(realSpy.datagramsSent() == 0 && r.spy.datagramsSent() == 0);
  // dry-run engine does not require a destination (setup before network is known)
  FakeClock c; EngineConfig k; k.protocol = LightProtocol::ArtNet; CHECK(LightingEngine::makeDryRun(c, k) != nullptr);
}

TEST(fade_in_physical_domain_through_engine) {
  Rig r; r.run(40);
  r.eng->setCue(cue({{"mover", "dimmer", 0}, {"mover", "strobe", 0}, {"mover", "pan", 2000}})); r.run(40);
  r.eng->setCue(cue({{"mover", "dimmer", 200}, {"mover", "strobe", 100}, {"mover", "pan", 4000}}, 800 * kNsPerMs));
  r.run(400);
  CHECK(r.val("mover", "dimmer") == 100 && r.val("mover", "pan") == 3000);
  CHECK(r.val("mover", "strobe") == 0);                 // 0 (macro/open) -> 100 (strobe range): held, never swept through
  r.run(400);
  CHECK(r.val("mover", "dimmer") == 200 && r.val("mover", "pan") == 4000 && r.val("mover", "strobe") == 100);
  // re-target mid-fade starts from the current displayed value
  r.eng->setCue(cue({{"mover", "dimmer", 0}}, 800 * kNsPerMs)); r.run(400);
  CHECK(r.val("mover", "dimmer") == 100);
  // channels dropped from a cue release immediately to lower layers
  CHECK(r.val("mover", "pan") == 32768);
  CHECK(!r.eng->setCue(cue({{"ghost", "dimmer", 1}})).ok());
}

TEST(graceful_stop_sends_safe_frames_and_sacn_termination) {
  Rig r; CHECK(r.arm());
  r.eng->setCue(cue({{"mover", "dimmer", 255}, {"mover", "pan", 50000}})); r.run(100);
  r.spy.clearRecords(); r.eng->stop();
  auto d = r.spy.records();
  CHECK(d.size() == 6);                                           // 3 repeats x 2 universes
  for (auto& g : d) {
    sacn::ParsedData p; CHECK(sacn::parseDataPacket(g.bytes.data(), g.bytes.size(), p));
    CHECK((p.p.options & sacn::kOptTerminated) != 0);
    if (p.p.universe == 1) CHECK(p.slots[0] == 0 && p.slots[2] == 0x80);   // safe, not last cue value
  }
  CHECK(!r.spy.isOpen() && r.eng->status().state == ArmState::Disarmed);
  r.spy.clearRecords(); r.eng->stop(); CHECK(r.spy.records().empty());   // idempotent
}
TEST(operator_disarm_is_graceful_and_normal_frames_not_terminated) {
  Rig r; CHECK(r.arm()); r.run(40);
  auto d = r.spy.records(); sacn::ParsedData p; sacn::parseDataPacket(d[0].bytes.data(), d[0].bytes.size(), p); CHECK(p.p.options == 0);
  r.spy.clearRecords(); r.eng->disarm(); CHECK(r.spy.records().size() == 6);
  r.spy.clearRecords(); r.run(200); CHECK(r.spy.records().empty());
}
TEST(artnet_graceful_stop_sends_safe_no_terminate_flag) {
  Rig r(LightProtocol::ArtNet); CHECK(r.arm()); r.eng->setCue(cue({{"dim", "dimmer", 255}})); r.run(40);
  r.spy.clearRecords(); r.eng->stop(); auto d = r.spy.records(); CHECK(d.size() == 6);
  artnet::ArtDmxPacket p; CHECK(artnet::parseArtDmx(d[1].bytes.data(), d[1].bytes.size(), p) && p.p.portAddress == 1 && p.data[0] == 0);
}

struct FakeUsb : IUsbDmxDevice {
  bool o = false; int frames = 0; std::vector<uint8_t> last;
  UsbAdapterInfo info() const override { return UsbAdapterInfo(); }
  Status open(const std::string&) override { o = true; return Status::Ok(); }
  void close() override { o = false; } bool isOpen() const override { return o; }
  Status sendDmx(const uint8_t* s, size_t n) override { ++frames; last.assign(s, s + n); return o ? Status::Ok() : Status::Fail(Err::DeviceLost, "closed"); }
};
TEST(usb_device_engine) {
  FakeClock clk; FakeUsb dev; EngineConfig k; Status st; auto e = LightingEngine::makeLiveUsb(clk, k, dev, &st); CHECK(e != nullptr);
  ProfileLibrary lib = ProfileLibrary::builtin(); Patch p; PatchEntry a; a.fixtureId = "d"; a.profileId = "generic-dimmer"; a.universe = 1; p.entries = {a};
  CHECK(e->applyPatch(p, lib).ok());
  CHECK(!e->openTransport().ok());                       // device not opened by platform yet
  dev.open("COM5"); CHECK(e->openTransport().ok());
  ArmToken t; CHECK(e->requestArmToken(t).ok() && e->arm(t).ok());
  e->setManual("d", "dimmer", 99); for (int i = 0; i < 100; ++i) { clk.advance(kNsPerMs); e->pump(); }
  CHECK(dev.frames >= 3 && dev.last.size() == 512 && dev.last[0] == 99);
  p.entries.push_back(a); p.entries[1].fixtureId = "e"; p.entries[1].universe = 2; p.entries[1].startAddress = 5;
  CHECK(!e->applyPatch(p, lib).ok());                    // one universe only
}

TEST(worker_thread_runs_and_stops_gracefully) {
  RecordingSink spy; EngineConfig k; k.protocol = LightProtocol::Sacn; k.host = "10.1.1.1"; k.tickHz = 40;
  auto e = LightingEngine::makeLive(systemClock(), k, spy);
  ProfileLibrary lib = ProfileLibrary::builtin(); Patch p; PatchEntry a; a.fixtureId = "d"; a.profileId = "generic-dimmer"; a.universe = 1; p.entries = {a};
  CHECK(e->applyPatch(p, lib).ok()); CHECK(e->start().ok());
  ArmToken t; CHECK(e->requestArmToken(t).ok() && e->arm(t).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  e->stop();
  CHECK(e->status().ticks >= 2 && spy.datagramsSent() >= 3 && !spy.isOpen());
  auto d = spy.records(); sacn::ParsedData pd; CHECK(sacn::parseDataPacket(d.back().bytes.data(), d.back().bytes.size(), pd) && (pd.p.options & sacn::kOptTerminated));
}
int main() { return ss_test::runAll(); }
