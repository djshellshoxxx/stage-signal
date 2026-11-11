// Lifecycle: ordered shutdown + thread join, threaded run, persistence/recovery, setup state, benchmark, adaptation.
#include "app_fakes.h"
#include "persistence/show_io.h"
using namespace apptest;

TEST(shutdown_runs_stages_in_spec_order_and_is_idempotent) {
  Rig r; r.ctl->setRecordingFolder("/rec"); r.ctl->selectAudio({"mic1", 48000, 0});
  show::ShowDocument d = testDoc(); d.patch.push_back(R"({"id":"fx1","profile":"generic-rgb","label":"w","universe":0,"address":1})");
  auto f = r.ctl->applyDocument(d); r.ctl->pump(); f.get(); r.run(100);
  r.act(ActionId::ArmLighting); r.run(100); r.act(ActionId::StartRecording); r.run(100);
  r.ctl->selectCamera({"cam1", "USB Cam"});
  CHECK(r.ctl->lightingArmed()); CHECK(r.ctl->status().recState == RecState::Recording);
  ShutdownReport rep = r.ctl->shutdown();
  std::vector<std::string> want = {"disarm_outputs", "broadcast", "recording", "quiesce_midi_lighting", "stop_capture", "drain_tasks", "destroy_workers", "renderer", "ui_close"};
  CHECK(rep.stages.size() == want.size());
  for (size_t i = 0; i < want.size() && i < rep.stages.size(); ++i) { CHECK(rep.stages[i].name == want[i]); CHECK(rep.stages[i].ok); }
  CHECK(rep.clean);
  CHECK(r.log.index("dmx.close") >= 0); CHECK(r.log.index("dmx.close") < r.log.index("audio.close"));   // outward disarm before capture stop
  CHECK(r.log.index("audio.close") < r.log.index("camera.close") || r.log.index("camera.close") > 0);
  CHECK(r.recFs.list().size() > 0);                                                                    // writers finalized
  ShutdownReport again = r.ctl->shutdown(); CHECK(again.stages.size() == want.size());
  CHECK(!r.ctl->running());
  Action a; a.id = ActionId::NextScene; CHECK(!r.ctl->submit(a).get().accepted);                       // after shutdown: rejected, not hung
}

TEST(threaded_controller_runs_and_joins_all_threads) {
  int before = threadCount();
  {
    Rig r(false, true);
    CHECK(r.ctl->selectAudio({"mic1", 48000, 0}).ok());
    r.assignMain("B");
    CHECK(threadCount() > before);
    for (int i = 0; i < 60; ++i) { r.audio.feedMs(10); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(waitFor([&] { return r.ctl->status().framesRendered > 5; }));
    FramePtr f; CHECK(waitFor([&] { return r.frame("main", f); }));
    Action a; a.id = ActionId::VisualBlackout; auto fut = r.ctl->submit(a);
    CHECK(fut.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready); CHECK(fut.get().accepted);
    CHECK(waitFor([&] { FramePtr g; return r.frame("main", g) && Rig::allZero(*g); }, 500));          // within ~a frame period or two
    r.audio.lose(); CHECK(waitFor([&] { return r.ctl->status().audioState == AudioState::Disconnected; }, 1000));
    ShutdownReport rep = r.ctl->shutdown(); CHECK(rep.clean);
    for (auto& s : rep.stages) if (!s.ok) std::printf("  stage %s: %s\n", s.name.c_str(), s.detail.c_str());
  }
  CHECK(waitFor([&] { return threadCount() <= before; }, 1000));                                       // no leaked threads
}

TEST(invalid_document_is_rejected_and_old_show_stays_live) {
  Rig r; auto f = r.ctl->applyDocument(testDoc()); r.ctl->pump(); CHECK(f.get().ok());
  show::ShowDocument bad = testDoc(); bad.scenes[0].rendererType = "nope";
  auto g = r.ctl->applyDocument(bad); r.ctl->pump(); Status s = g.get(); CHECK(!s.ok());
  CHECK(r.ctl->document()->name == "App Test Show"); CHECK(r.ctl->document()->scenes[0].rendererType == "spectrum_bars");
  auto h = r.ctl->openShow(r.dir.path("missing.json")); r.ctl->pump(); CHECK(!h.get().ok());
  CHECK(r.ctl->document()->name == "App Test Show");
}

TEST(save_open_roundtrip_keeps_overrides_and_midi_mappings_and_is_atomic) {
  Rig r; auto f = r.ctl->applyDocument(testDoc()); r.ctl->pump(); f.get();
  r.act(ActionId::SetBpm, 133); r.act(ActionId::SetKey, 9 + 12);
  MidiMapping m; m.deviceId = "in1"; m.channel = 0; m.type = MidiMsgType::CC; m.number = 9; m.action = ActionId::Speed; m.mode = MidiMode::Absolute; m.modeConfirmed = true; m.scaleMin = 0.25; m.scaleMax = 4;
  CHECK(!r.ctl->midiMappings().add(m).empty());
  r.run(1100);                                                                // housekeeping notices the mapping edit
  CHECK(r.ctl->showDirty());
  std::string path = r.dir.path("show.stagesignal.json");
  auto sv = r.ctl->saveShow(path); r.ctl->pump(); r.ctl->pump(); Status s = sv.get(); CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str());
  CHECK(!r.ctl->showDirty());
  Rig r2; auto op = r2.ctl->openShow(path); r2.ctl->pump(); r2.ctl->pump(); s = op.get(); CHECK(s.ok());
  r2.run(50);
  AppStatus st = r2.ctl->status(); CHECK(st.bpmLabel == "Manual"); CHECK_NEAR(st.bpm, 133, 0.01); CHECK(st.keyName == "A minor"); CHECK(st.midiMappings == 1);
  CHECK(!st.dmxArmed); CHECK(st.recState == RecState::Idle); CHECK(st.liveState == BroadcastState::Off);
}

TEST(portable_export_import_strips_devices_and_loads) {
  Rig r; show::ShowDocument d = testDoc(); auto f = r.ctl->applyDocument(d); r.ctl->pump(); f.get();
  std::string zip = r.dir.path("x.stagesignal");
  auto e = r.ctl->exportPortable(zip); r.ctl->pump(); r.ctl->pump(); Status s = e.get(); CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str());
  Rig r2; auto i = r2.ctl->importPortable(zip, r2.dir.path("imported")); r2.ctl->pump(); r2.ctl->pump(); s = i.get(); CHECK(s.ok());
  CHECK(r2.ctl->document()->name == "App Test Show");
  auto bad = r2.ctl->importPortable(r2.dir.path("nonexistent.zip"), r2.dir.path("imp2")); r2.ctl->pump(); CHECK(!bad.get().ok());
  CHECK(r2.ctl->document()->name == "App Test Show");
}

TEST(crash_recovery_restores_idle_and_disarmed_and_offers_not_applies) {
  Rig r(true, true, [](AppConfig& c) { c.autosaveInterval = 2 * kNsPerSec; });
  show::ShowDocument d = testDoc(); d.name = "Recover Me"; d.patch.push_back(R"({"id":"fx1","profile":"generic-rgb","label":"w","universe":0,"address":1})");
  auto f = r.ctl->applyDocument(d); r.ctl->pump(); f.get();
  r.run(4500); r.ctl->pump();                                                     // autosave fires (dirty > interval)
  r.ctl->shutdown();
  persist::CrashRecovery cr(persist::defaultFileOps(), r.dir.path("data")); cr.beginSession(1);   // model: process died, marker left behind
  r.make(); CHECK(r.ctl->start().ok());
  persist::RecoveryInfo ri = r.ctl->recoveryInfo(); CHECK(ri.crashed); CHECK(ri.autosaveValid);
  CHECK(r.ctl->document()->name == "Untitled show");                              // offered, never silently applied
  auto rs = r.ctl->restoreRecovery(r.dir.path("recovered.json")); r.ctl->pump(); r.ctl->pump(); Status s = rs.get(); CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str());
  r.run(50);
  CHECK(r.ctl->document()->name == "Recover Me");
  AppStatus st = r.ctl->status(); CHECK(st.idleActive); CHECK(st.recovered); CHECK(!st.dmxArmed); CHECK(st.recState == RecState::Idle); CHECK(st.liveState == BroadcastState::Off);
  CHECK(!r.ctl->showSnapshot().lightingArmRequested); CHECK(r.sink.opens() == 0);
  CHECK(!r.ctl->recoveryInfo().crashed);
}

TEST(setup_state_wizard_saved_as_local_settings_and_restored) {
  Rig r; SetupState s0 = r.ctl->setupState(); CHECK(!s0.complete()); CHECK(!s0.gainNote.empty());
  r.assignMain("B");
  CHECK(r.ctl->selectAudio({"mic1", 48000, 2 - 2}).ok()); r.ctl->acknowledgeGainNote(true);
  CHECK(r.ctl->selectCamera({"cam1", "USB Cam"}).ok());
  CHECK(r.ctl->selectMidiInput({"in1", "Controller", true}).ok()); CHECK(r.ctl->selectMidiOutput({"out1", "Synth", false}).ok());
  LightingInterface li; li.host = "192.168.1.50"; CHECK(r.ctl->setLightingInterface(li).ok());
  CHECK(r.ctl->setRecordingFolder("/rec").ok());
  SetupState s = r.ctl->setupState();
  CHECK(s.audioDeviceId == "mic1"); CHECK(s.gainAcknowledged); CHECK(s.cameraPreviewed); CHECK(s.lightingConfigured); CHECK(s.recordingFolder == "/rec");
  CHECK(!s.cameraPreviewed || !r.ctl->status().cameraPublished);                    // setup never publishes the camera
  CHECK(!r.ctl->status().dmxArmed);
  CHECK(!s.complete());                                                             // benchmark still outstanding
  persist::LocalSettings ls = r.ctl->localSettings();
  CHECK(ls.devices["audioInput"] == "mic1"); CHECK(ls.outputDisplays["main"] == "B"); CHECK(ls.recordingFolder == "/rec");
  for (auto& kv : ls.devices) CHECK(kv.second.find("password") == std::string::npos);
  r.ctl->shutdown(); r.make(); CHECK(r.ctl->start().ok()); r.run(100);
  for (auto& v : r.ctl->outputRoutes()) if (v.logicalId == "main") { CHECK(v.state == RouteState::Active); CHECK(v.displayId == "B"); }
  CHECK(r.ctl->status().audioState == AudioState::Running);
  CHECK(r.ctl->status().cameraState == CameraState::Closed);                         // camera is not auto-opened on launch
}

TEST(benchmark_runs_on_mock_sinks_writes_json_and_marks_setup) {
  Rig r; r.ctl->setRecordingFolder("/rec");
  show::ShowDocument d = testDoc(); d.patch.push_back(R"({"id":"fx1","profile":"generic-rgb","label":"w","universe":0,"address":1})");
  auto f = r.ctl->applyDocument(d); r.ctl->pump(); f.get();
  BenchmarkRequest q; q.paceRealTime = false; q.fakeClock = &r.clk; q.durationScale = 0.01; q.width = 160; q.height = 120;
  q.scenarios = {BenchId::BEN01, BenchId::BEN02, BenchId::BEN03, BenchId::BEN04, BenchId::BEN05};
  auto fut = r.ctl->runBenchmark(q); r.ctl->pump(); r.ctl->pump();
  BenchmarkOutcome o = fut.get();
  CHECK(o.status.ok()); CHECK(o.result.scaled); CHECK(!o.result.qualifying); CHECK(o.result.isolationOk);
  CHECK(o.result.scenarios.size() == 5); for (auto& s : o.result.scenarios) { CHECK(s.status == "completed"); CHECK(s.isolationViolations == 0); }
  CHECK(std::filesystem::exists(o.jsonPath)); CHECK(r.sink.datagramsSent() == 0); CHECK(r.sink.opens() == 0);
  CHECK(r.recFs.list().empty());                                                      // no recording files either
  SetupState st = r.ctl->setupState(); CHECK(st.benchmarkDone); CHECK(!st.benchmarkQualifying);
  AppStatus as = r.ctl->status(); CHECK(as.recState == RecState::Idle); CHECK(as.liveState == BroadcastState::Off); CHECK(!as.dmxArmed);
  // refused while live outputs run
  r.ctl->selectAudio({"mic1", 48000, 0}); r.run(100); CHECK(r.act(ActionId::StartRecording).accepted);
  auto f2 = r.ctl->runBenchmark(q); BenchmarkOutcome o2 = f2.get(); CHECK(!o2.status.ok()); CHECK(o2.status.code == Err::PermissionDenied);
  CapabilityStatus cs = r.ctl->capability(); CHECK(!cs.decisions.empty());
}

TEST(adaptation_applies_ladder_to_fps_scale_preview_and_locked_never_changes) {
  Rig r; r.cpu.cur.available = true; r.cpu.cur.logicalCpus = 1; r.cpu.cur.processPct = 95; r.cpu.cur.systemPct = 95;
  CHECK(r.ctl->status().adaptationLevel == 0); CHECK_NEAR(r.ctl->status().previewFps, 10, 0.01);
  r.run(7000); AppStatus s = r.ctl->status();
  CHECK(s.adaptationLevel >= 1); CHECK_NEAR(s.previewFps, 5, 0.01); CHECK(s.adaptationStep != "none");
  r.run(30000); s = r.ctl->status();
  CHECK(s.adaptationLevel >= 3); CHECK(s.renderScale < 1.0); CHECK(s.sceneFps < 25.0);
  bool w = false; for (auto& x : s.warnings) if (x.id == "adaptation") w = true; CHECK(w);
  r.cpu.cur.processPct = 20; r.cpu.cur.systemPct = 20; int lvl = s.adaptationLevel; r.run(70000);
  CHECK(r.ctl->status().adaptationLevel < lvl);                                       // restores one step per >=30 s
  Rig l; l.ctl->setQualityLocked(true); l.cpu.cur.available = true; l.cpu.cur.processPct = 99; l.cpu.cur.systemPct = 99; l.cpu.cur.logicalCpus = 1;
  l.run(20000); AppStatus ls = l.ctl->status(); CHECK(ls.adaptationLevel == 0); CHECK(ls.qualityLocked);
  bool lw = false; for (auto& x : ls.warnings) if (x.id == "overload_locked") lw = true; CHECK(lw);
}
int main() { return ss_test::runAll(); }
