// Safety invariants: DMX isolation/arming, recording/broadcast start rules, audio loss, display unplug, MIDI loss, secrets.
#include "app_fakes.h"
#include <fstream>
#include <sstream>
using namespace apptest;

static show::ShowDocument lightDoc() {
  show::ShowDocument d = testDoc();
  d.patch.push_back(R"({"id":"fx1","profile":"generic-rgb","label":"wash","universe":0,"address":1})");
  return d;
}
static void load(Rig& r, const show::ShowDocument& d) { auto f = r.ctl->applyDocument(d); r.ctl->pump(); Status s = f.get(); CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str()); }

TEST(lighting_preview_transmits_zero_datagrams_until_explicit_arm) {
  Rig r; load(r, lightDoc());
  r.run(500);
  CHECK(r.sink.datagramsSent() == 0); CHECK(r.sink.records().empty()); CHECK(r.sink.opens() == 0);
  CHECK(r.ctl->lightingDatagramsPreviewed() > 0);                         // dry-run engine computes frames
  CHECK(!r.ctl->lightingArmed()); CHECK(r.ctl->status().dmxText == "Off");
  CHECK(!r.act(ActionId::ArmLighting, 0, ActionSource::Show).accepted);   // show cues can never arm
  CHECK(!r.act(ActionId::ArmLighting, 0, ActionSource::System).accepted);
  r.run(100); CHECK(r.sink.datagramsSent() == 0);
  ActionResult a = r.act(ActionId::ArmLighting);
  CHECK(a.accepted); if (!a.accepted) std::printf("  %s\n", a.reason.c_str());
  r.run(300);
  CHECK(r.ctl->lightingArmed()); CHECK(r.ctl->status().dmxArmed); CHECK(r.ctl->status().dmxText == "Armed");
  CHECK(r.sink.datagramsSent() > 0);
}

TEST(lighting_blackout_and_disarm_and_reload_stay_safe) {
  Rig r; load(r, lightDoc()); CHECK(r.act(ActionId::ArmLighting).accepted); r.run(200);
  CHECK(r.act(ActionId::LightingBlackout).accepted); r.run(200);
  CHECK(r.ctl->status().lightingBlackout);
  CHECK(r.act(ActionId::DisarmLighting).accepted); r.run(100);
  CHECK(!r.ctl->lightingArmed()); uint64_t n = r.sink.datagramsSent(); r.run(300); CHECK(r.sink.datagramsSent() == n);
  CHECK(r.act(ActionId::ArmLighting).accepted); r.run(100); CHECK(r.ctl->lightingArmed());
  load(r, lightDoc()); r.run(100);                                          // loading a show disarms; never re-arms
  CHECK(!r.ctl->lightingArmed()); CHECK(!r.ctl->showSnapshot().lightingArmRequested);
}

TEST(lighting_arm_refused_without_valid_patch) {
  Rig r; ActionResult a = r.act(ActionId::ArmLighting);
  CHECK(!a.accepted); CHECK(!r.ctl->lightingArmed()); CHECK(!r.ctl->showSnapshot().lightingArmRequested); CHECK(r.sink.opens() == 0);
}

TEST(lighting_fault_disarms_and_does_not_rearm) {
  Rig r; load(r, lightDoc()); CHECK(r.act(ActionId::ArmLighting).accepted); r.run(100);
  r.sink.setFailing(true); r.run(300);
  CHECK(!r.ctl->lightingArmed());
  r.sink.setFailing(false); r.run(300); CHECK(!r.ctl->lightingArmed()); CHECK(!r.ctl->showSnapshot().lightingArmRequested);
}

TEST(audio_loss_reports_within_one_second_and_never_switches_device) {
  Rig r; CHECK(r.ctl->selectAudio({"mic1", 48000, 0}).ok()); r.run(200);
  CHECK(r.ctl->status().audioState == AudioState::Running);
  int opens = r.audio.opens; r.audio.lose();
  CHECK(r.ctl->status().audioState == AudioState::Disconnected);            // immediately visible (<1 s)
  r.run(1500, 0.8, false);
  AppStatus s = r.ctl->status(); CHECK(s.audioState == AudioState::Disconnected);
  CHECK(r.audio.opens == opens);                                            // no auto-selection, not even a reopen of the same id
  CHECK(r.audio.openedId == "mic1");
  bool w = false; for (auto& x : s.warnings) if (x.id == "audio_lost") { w = true; CHECK(x.critical); } CHECK(w);
  CHECK(s.meterPeakDb[0] <= kDbSilence + 1);
  CHECK(r.ctl->reopenAudio().ok()); r.run(100); CHECK(r.ctl->status().audioState == AudioState::Running);
}

TEST(audio_loss_policy_idle_engages_within_two_seconds) {
  Rig r; show::ShowDocument d = testDoc(); d.idle.audioLoss = show::AudioLossPolicy::Idle; d.idle.audioLossMs = 500; d.idle.screen = "black";
  load(r, d); r.ctl->selectAudio({"mic1", 48000, 0}); r.run(300);
  CHECK(!r.ctl->showSnapshot().idleActive);
  r.audio.lose(); r.run(500, 0.8, false); CHECK(r.ctl->showSnapshot().idleActive); CHECK(r.ctl->status().idleActive);
  r.assignMain("B"); r.run(100); FramePtr f; CHECK(r.frame("main", f)); CHECK(Rig::allZero(*f));
}

TEST(saved_audio_device_reopens_only_that_device_after_restart) {
  Rig r; r.ctl->selectAudio({"mic2", 48000, 0}); r.ctl->shutdown();
  r.audio.devs[1].id = "gone"; r.audio.openLog.clear(); r.make(); CHECK(r.ctl->start().ok());
  CHECK(r.audio.openLog.size() == 1); CHECK(r.audio.openLog[0] == "mic2");
  CHECK(r.ctl->status().audioState == AudioState::Unavailable);
  bool w = false; for (auto& x : r.ctl->status().warnings) if (x.id == "audio_open") w = true; CHECK(w);
}

TEST(display_unplug_marks_disconnected_controls_untouched_and_reattach_by_identity) {
  Rig r; r.assignMain("B"); r.run(200);
  FramePtr f; CHECK(r.frame("main", f));
  r.disp.set({r.disp.list[0], r.disp.list[2]});                              // B unplugged
  r.run(1100);
  auto v = r.ctl->outputRoutes(); const OutputRouteView* m = nullptr; const OutputRouteView* c = nullptr;
  for (auto& x : v) { if (x.logicalId == "main") m = &x; if (x.logicalId == "controls") c = &x; }
  CHECK(m && m->state == RouteState::Disconnected); CHECK(c && c->state == RouteState::ControlsHost && c->displayId == "A");
  CHECK(!r.frame("main", f));                                                // no stale frame for a dead window
  bool warn = false; for (auto& w : r.ctl->status().warnings) if (w.id == "route:main") warn = true; CHECK(warn);
  r.disp.set({r.disp.list[0], r.disp.list[1], FakeDisplays::disp("B", 5, 1700, 320, 240)});
  r.run(200);
  for (auto& x : r.ctl->outputRoutes()) if (x.logicalId == "main") { CHECK(x.state == RouteState::Active); CHECK(x.displayId == "B"); }
  CHECK(r.frame("main", f));
}

TEST(identical_monitors_after_restart_stay_idle_until_reassigned) {
  Rig r; r.disp.list = {FakeDisplays::disp("A", 0, 0, 800, 600, true), FakeDisplays::disp("M", 1, 800, 320, 240, false, true), FakeDisplays::disp("M", 2, 1120, 320, 240, false, true)};
  r.ctl->refreshDisplays();
  CHECK(r.ctl->assignOutput("main", r.disp.list[2]).ok()); r.run(100);
  FramePtr f; CHECK(r.frame("main", f));
  r.ctl->shutdown(); r.make(); CHECK(r.ctl->start().ok()); r.run(100);
  for (auto& x : r.ctl->outputRoutes()) if (x.logicalId == "main") CHECK(x.state == RouteState::NeedsReassign);
  CHECK(!r.frame("main", f));
}

TEST(never_auto_start_recording_broadcast_or_dmx) {
  Rig r; load(r, lightDoc()); r.run(100);
  auto st = [&] { AppStatus s = r.ctl->status(); return s.recState == RecState::Idle && s.liveState == BroadcastState::Off && !s.dmxArmed && !s.cameraPublished; };
  CHECK(st());
  auto b = r.ctl->runBenchmark([] { BenchmarkRequest q; return q; }());
  (void)b;
}

TEST(recording_requires_operator_folder_and_audio_and_runs_markers) {
  Rig r;
  CHECK(!r.act(ActionId::StartRecording, 0, ActionSource::Show).accepted);
  ActionResult a = r.act(ActionId::StartRecording); CHECK(!a.accepted); CHECK(a.reason.find("folder") != std::string::npos);
  CHECK(r.ctl->setRecordingFolder("/rec").ok());
  a = r.act(ActionId::StartRecording); CHECK(!a.accepted); CHECK(r.ctl->status().recState != RecState::Recording);   // no audio running
  CHECK(r.ctl->selectAudio({"mic1", 48000, 0}).ok()); r.run(100);
  a = r.act(ActionId::StartRecording); CHECK(a.accepted); if (!a.accepted) std::printf("  %s\n", a.reason.c_str());
  CHECK(r.ctl->status().recState == RecState::Recording);
  r.run(500); CHECK(r.act(ActionId::AddMarker).accepted);
  RecordingStatus rs = r.ctl->recordingStatus(); CHECK(rs.markers == 1); CHECK(rs.inputBlocks > 0);
  CHECK(r.ctl->status().recElapsedSec > 0.2);
  CHECK(r.act(ActionId::StopRecording).accepted); r.run(50);
  CHECK(waitFor([&] { r.ctl->pump(); return r.ctl->status().recState == RecState::Completed; }));
  bool marker = false; for (auto& e : r.ctl->journalSnapshot()) if (e.kind == JournalKind::Marker) marker = true; CHECK(marker);
}

static BroadcastSettings rtpSettings() {
  BroadcastSettings s = defaultBroadcastSettings();
  s.endpoint.protocol = BroadcastProtocol::RTP; s.endpoint.host = "192.168.1.20"; s.endpoint.port = 5004; s.endpoint.path.clear(); s.endpoint.streamKeyRef.clear();
  s.videoCodec = VideoCodec::MJPEG; s.width = 160; s.height = 120; s.fps = 25; return s;
}

TEST(broadcast_only_with_user_token_and_never_from_show_load_or_cues) {
  Rig r; CHECK(r.ctl->configureBroadcast(rtpSettings()).ok());
  r.ctl->selectAudio({"mic1", 48000, 0}); r.assignMain("B"); r.run(100);
  CHECK(!r.act(ActionId::StartBroadcast, 0, ActionSource::Show).accepted);
  CHECK(!r.act(ActionId::StartBroadcast, 0, ActionSource::System).accepted);
  CHECK(r.ctl->broadcastStatus().state == BroadcastState::Off);
  UserStartToken none; CHECK(!r.ctl->startBroadcast(std::move(none)).ok());
  load(r, testDoc()); r.run(100); CHECK(r.ctl->broadcastStatus().state == BroadcastState::Off);
  UserStartToken tok; CHECK(r.ctl->startGate().issue(ActionSource::UI, tok).ok());
  Status s = r.ctl->startBroadcast(std::move(tok)); CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str());
  r.run(600);
  BroadcastStatus bs = r.ctl->broadcastStatus(); CHECK(bs.state == BroadcastState::Live || bs.state == BroadcastState::Connecting);
  CHECK(bs.packetsSent > 0); CHECK(r.lastTransport && r.lastTransport->received().size() > 0);
  CHECK(r.ctl->status().liveState == bs.state);
  r.ctl->stopBroadcast(); CHECK(r.ctl->broadcastStatus().state == BroadcastState::Off);
}

TEST(broadcast_queues_stay_bounded_when_receiver_stalls) {
  Rig r; r.ctl->configureBroadcast(rtpSettings()); r.ctl->selectAudio({"mic1", 48000, 0}); r.assignMain("B"); r.run(50);
  UserStartToken tok; r.ctl->startGate().issue(ActionSource::UI, tok);
  CHECK(r.ctl->startBroadcast(std::move(tok)).ok());
  r.lastTransport->acceptLimit = 3;                                         // receiver backpressure
  r.run(8000);
  AppStatus s = r.ctl->status(); BroadcastStatus bs = r.ctl->broadcastStatus();
  CHECK(bs.queueDepth <= 512); CHECK(bs.packetsDropped > 0); CHECK(s.castAudioQueueDepth <= 300);
  r.ctl->stopBroadcast();
}

struct FakeH264 : IVideoEncoder {
  Status open(const VideoProfile&) override { return Status::Ok(); }
  Status encode(const Frame&, Ns pts, EncodedPacket& o, bool& have) override { o.pts = pts; o.key = true; o.data = {1, 2, 3}; have = true; return Status::Ok(); }
  void close() override {} Ns delayNs() const override { return 0; } std::string name() const override { return "fake-h264"; } VideoCodec codec() const override { return VideoCodec::H264; }
};
TEST(secrets_only_via_secret_store_never_in_logs_journal_or_bundle) {
  EncoderRegistry reg = EncoderRegistry::withBuiltins(); reg.add(VideoCodec::H264, "fake-h264", [] { return std::unique_ptr<IVideoEncoder>(new FakeH264()); }, true);
  Rig r; r.deps.encoders = &reg; r.ctl->shutdown(); r.make(); r.ctl->start();
  const std::string key = "live_SuperSecretStreamKey_12345";
  CHECK(r.ctl->storeSecret("streamkey", key).ok()); CHECK(r.secrets.m["streamkey"] == key);
  BroadcastSettings s = defaultBroadcastSettings(); s.endpoint.protocol = BroadcastProtocol::RTMPS; s.endpoint.host = "ingest.example.com"; s.endpoint.path = "/app"; s.endpoint.streamKeyRef = "streamkey";
  CHECK(r.ctl->configureBroadcast(s).ok());
  CHECK(r.ctl->localSettings().secretRefs.count("streamkey") == 1);
  for (auto& kv : r.ctl->localSettings().secretRefs) CHECK(kv.second.find(key) == std::string::npos);
  r.run(50); UserStartToken tok; r.ctl->startGate().issue(ActionSource::UI, tok);
  Status st = r.ctl->startBroadcast(std::move(tok)); r.run(300); if (!st.ok()) std::printf("  start: %s\n", st.detail.c_str());
  CHECK(r.lastTransport && r.lastTransport->lastSecretSeen() == key);       // reached the transport only
  r.ctl->addMarker("note " + key);                                          // even deliberate leaks are redacted
  std::string dest = r.dir.path("bundle.zip");
  CHECK(r.ctl->exportSupportBundle(dest, {"logs", "journal", "system", "adaptation"}).ok());
  r.ctl->shutdown();
  auto slurp = [](const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
  CHECK(slurp(dest).find(key) == std::string::npos);
  for (auto& e : std::filesystem::recursive_directory_iterator(r.dir.path("data"))) if (e.is_regular_file()) CHECK(slurp(e.path().string()).find(key) == std::string::npos);
  (void)st;
}

TEST(midi_mapping_drives_show_and_port_loss_marks_unavailable_without_stale_actions) {
  Rig r; MidiMapping m; m.deviceId = "in1"; m.channel = 0; m.type = MidiMsgType::CC; m.number = 7; m.action = ActionId::Brightness; m.mode = MidiMode::Absolute; m.modeConfirmed = true;
  CHECK(!r.ctl->midiMappings().add(m).empty());
  CHECK(r.ctl->selectMidiInput({"in1", "Controller", true}).ok());
  r.midi.injectInput({0xB0, 7, 127}); r.run(50);
  CHECK_NEAR(r.ctl->showSnapshot().outputs[0].brightness, 1.0, 0.01);
  r.midi.injectInput({0xB0, 7, 0}); r.run(50); CHECK_NEAR(r.ctl->showSnapshot().outputs[0].brightness, 0.0, 0.01);
  r.midi.simulateLoss(true);
  CHECK(r.ctl->midiMappings().status(r.ctl->midiMappings().list()[0].id) == MappingStatus::DeviceUnavailable);   // immediate
  r.run(1200); AppStatus s = r.ctl->status(); CHECK(!s.midiInputAlive); CHECK(s.midiMappingsUnavailable == 1);
  bool w = false; for (auto& x : s.warnings) if (x.id == "midi_in") w = true; CHECK(w);
  r.midi.restore(); CHECK(r.ctl->reconnectMidi().ok()); r.run(50);
  CHECK(r.ctl->status().midiInputAlive); CHECK_NEAR(r.ctl->showSnapshot().outputs[0].brightness, 0.0, 0.01);   // no stale action replayed
  r.midi.injectInput({0xB0, 7, 127}); r.run(50); CHECK_NEAR(r.ctl->showSnapshot().outputs[0].brightness, 1.0, 0.01);
}

TEST(midi_output_clock_follows_effective_bpm_and_feedback_route_starts_disabled) {
  Rig r; CHECK(r.ctl->selectMidiOutput({"out1", "Synth", false}).ok());
  MidiRoute rt; CHECK(r.ctl->midiRoutes().get("out:feedback", rt)); CHECK(!rt.enabled);
  r.act(ActionId::SetBpm, 120); r.ctl->setMidiClockEnabled(true); r.midi.clearSent();
  r.run(1000);
  size_t pulses = 0; for (auto& s : r.midi.sent()) if (s.bytes.size() == 1 && s.bytes[0] == 0xF8) ++pulses;
  CHECK(pulses >= 44 && pulses <= 52);                                      // 24 ppqn at 120 BPM = 48/s
  CHECK(r.ctl->status().bpmLabel == "Manual");
}
int main() { return ss_test::runAll(); }
