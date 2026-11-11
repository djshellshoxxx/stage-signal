// AppController pipeline (fakes, FakeClock, manual pump): audio->features->scene, acks, queues, blackout, camera, text, outputs.
#include "app_fakes.h"
using namespace apptest;

TEST(audio_beats_reach_features_meters_and_scene_changes) {
  Rig r; r.ctl->applyDocument(testDoc()); r.ctl->pump();
  CHECK(r.ctl->selectAudio({"mic1", 48000, 0}).ok());
  r.run(100);
  int first = r.ctl->showSnapshot().sceneIndex;
  r.run(3000);
  FeatureSnapshot f = r.ctl->features();
  CHECK(f.onsetCount > 0); CHECK(f.rmsDbMix > -60.0f); CHECK(!f.silent);
  AppStatus s = r.ctl->status();
  CHECK(s.audioState == AudioState::Running); CHECK(s.audioDevice == "Studio Mic"); CHECK(s.meterPeakDb[0] > -40.0f);
  CHECK(!s.audioLossPolicyEngaged);
  auto j = r.ctl->journalSnapshot(); int sc = 0; for (auto& e : j) if (e.kind == JournalKind::Scene) ++sc;
  CHECK(sc >= 1);                                         // dwell elapsed with live audio => scene change
  (void)first;
}

TEST(action_acks_accept_reject_and_queue_bounds) {
  Rig r;
  ActionResult ok = r.act(ActionId::NextScene); CHECK(ok.accepted); CHECK(ok.revision > 0);
  ActionResult bad = r.act(ActionId::SelectScene, 99); CHECK(!bad.accepted); CHECK(!bad.reason.empty());
  std::vector<std::future<ActionResult>> fs; int immediate = 0;
  for (int i = 0; i < 300; ++i) { Action a; a.id = ActionId::NextScene; fs.push_back(r.ctl->submit(a)); if (fs.back().wait_for(std::chrono::seconds(0)) == std::future_status::ready) ++immediate; }
  CHECK(immediate == 300 - 256);                          // beyond 256 pending: rejected at the door, never blocking
  CHECK(r.ctl->status().actionQueueDepth == 256);
  int rejected = 0;
  for (int i = 0; i < 2; ++i) r.ctl->pump();
  for (auto& f : fs) { ActionResult x = f.get(); if (!x.accepted && x.reason == "action queue full") ++rejected; }
  CHECK(rejected == 44); CHECK(r.ctl->status().actionsRejectedFull == 44);
  // a critical action still gets through while the normal queue is full
  for (int i = 0; i < 300; ++i) { Action a; a.id = ActionId::NextScene; r.ctl->submit(a, nullptr); }
  Action b; b.id = ActionId::VisualBlackout; auto bf = r.ctl->submit(b); r.ctl->pump(); CHECK(bf.get().accepted);
}

TEST(capture_queue_bounded_under_stalled_consumer) {
  Rig r; CHECK(r.ctl->selectAudio({"mic1", 48000, 0}).ok());
  for (int i = 0; i < 200; ++i) r.audio.feedMs(10);       // 2 s of audio with the analysis worker stalled
  AppStatus s = r.ctl->status();
  CHECK(s.captureQueueDepth <= 25); CHECK(s.captureDrops > 0); CHECK(s.captureOverruns > 0);
  r.run(100); CHECK(r.ctl->status().captureQueueDepth == 0);
}

TEST(blackout_reaches_output_frames_by_next_frame_and_restore_keeps_scene) {
  Rig r; r.assignMain("B"); r.run(300);
  FramePtr f; CHECK(r.frame("main", f)); CHECK(Rig::anyNonZero(*f)); CHECK(f->w == 320 && f->h == 240);
  int scene = r.ctl->showSnapshot().sceneIndex;
  Action a; a.id = ActionId::VisualBlackout; auto fut = r.ctl->submit(a);
  r.clk.advance(40 * kNsPerMs); r.ctl->pump(); CHECK(fut.get().accepted);
  CHECK(r.frame("main", f)); CHECK(Rig::allZero(*f));
  CHECK(r.ctl->status().visualBlackout); CHECK(r.ctl->outputRoutes()[0].blackedOut);
  FramePtr pv; CHECK(r.ctl->previewFrame(pv)); CHECK(Rig::allZero(*pv));
  r.run(200); CHECK(r.frame("main", f)); CHECK(Rig::allZero(*f));                       // latched
  CHECK(r.act(ActionId::RestoreBlackout, 0).accepted); r.run(100);
  CHECK(r.frame("main", f)); CHECK(Rig::anyNonZero(*f)); CHECK(r.ctl->showSnapshot().sceneIndex == scene);
}

TEST(controls_role_output_never_gets_frames) {
  Rig r; r.assignMain("B"); r.run(200);
  FramePtr f; CHECK(!r.frame("ctl", f));
  for (auto& v : r.ctl->outputRoutes()) if (v.logicalId == "ctl") CHECK(v.state == RouteState::ControlsHost);
}

TEST(duplicate_outputs_share_one_composition_independent_gets_own) {
  Rig r; { auto f = r.ctl->applyDocument(testDoc()); r.ctl->pump(); CHECK(f.get().ok()); }
  r.disp.list[2].w = 320; r.ctl->assignOutput("main", r.disp.list[1]); r.ctl->assignOutput("alt", r.disp.list[2]); r.ctl->refreshDisplays();
  r.run(200);
  FramePtr a, b; CHECK(r.frame("main", a)); CHECK(r.frame("alt", b)); if (!a || !b) return;
  CHECK(a.get() == b.get());                                                  // duplicate route: same composition object
  CHECK(r.act(ActionId::SetTarget, 1).accepted); CHECK(r.act(ActionId::SelectScene, 3, ActionSource::UI, 1).accepted);
  r.run(200); CHECK(r.frame("main", a)); CHECK(r.frame("alt", b)); if (!a || !b) return;
  CHECK(a.get() != b.get()); CHECK(a->px != b->px);
  bool ind = false; for (auto& v : r.ctl->outputRoutes()) if (v.logicalId == "alt") ind = v.independentScene; CHECK(ind);
}

TEST(audience_shows_no_diagnostics_unless_enabled) {
  Rig r; r.assignMain("B"); r.run(200);
  CHECK(!r.ctl->outputRoutes()[0].diagnostics);
  FramePtr off, on; CHECK(r.frame("main", off)); Frame copy = *off;
  r.ctl->setOutputDiagnostics("main", true); r.run(200); CHECK(r.frame("main", on));
  CHECK(r.ctl->outputRoutes()[0].diagnostics);
  CHECK(copy.px != on->px);
}

TEST(camera_preview_never_reaches_audience_until_published) {
  Rig r; r.assignMain("B");
  CHECK(r.ctl->selectCamera({"cam1", "USB Cam"}).ok()); r.run(300);
  FramePtr f, cp; CHECK(r.frame("main", f));
  auto hasGreen = [](const Frame& fr) { for (auto p : fr.px) if (p == 0x00FF00) return true; return false; };
  CHECK(!hasGreen(*f));                                                       // preview only
  CHECK(r.ctl->cameraPreviewFrame(cp)); CHECK(hasGreen(*cp));
  CHECK(r.ctl->status().cameraState == CameraState::Live); CHECK(!r.ctl->status().cameraPublished);
  CameraPublication pub; pub.published = true; r.ctl->publishCamera(pub); r.run(200);
  CHECK(r.frame("main", f)); CHECK(hasGreen(*f)); CHECK(r.ctl->status().cameraPublished);
  r.ctl->selectCamera({"cam2", "Other Cam"});                                 // selecting another camera must not publish it
  CHECK(!r.ctl->cameraPublication().published); r.run(200); CHECK(r.frame("main", f)); CHECK(!hasGreen(*f));
}

TEST(camera_loss_hides_layer_after_freeze_and_never_switches) {
  Rig r; r.assignMain("B"); r.ctl->selectCamera({"cam1", "USB Cam"}); CameraPublication p; p.published = true; r.ctl->publishCamera(p); r.run(200);
  auto hasGreen = [](const Frame& fr) { for (auto q : fr.px) if (q == 0x00FF00) return true; return false; };
  FramePtr f; CHECK(r.frame("main", f)); CHECK(hasGreen(*f));
  int opens = r.cam.opens; r.cam.lose(); r.run(200); CHECK(r.frame("main", f)); CHECK(hasGreen(*f));   // frozen < 500 ms
  r.run(600); CHECK(r.frame("main", f)); CHECK(!hasGreen(*f));
  CHECK(r.cam.opens == opens); CHECK(r.ctl->status().cameraState == CameraState::Lost);
}

TEST(text_draft_preview_publish_pipeline) {
  Rig r; r.assignMain("B");
  auto d = r.ctl->setDraftText("Hello Stage"); r.ctl->pump(); CHECK(d.get().ok());
  r.run(100); CHECK(r.ctl->showSnapshot().outputs[0].publishedText.empty());
  auto p = r.ctl->previewDraftText(); r.ctl->pump(); CHECK(p.get().ok()); CHECK(r.ctl->showSnapshot().outputs[0].publishedText.empty());
  FramePtr before; r.frame("main", before); Frame b0 = *before;
  auto u = r.ctl->publishDraftText(-1); r.ctl->pump(); CHECK(u.get().ok()); r.run(100);
  CHECK(r.ctl->showSnapshot().outputs[0].publishedText == "Hello Stage");
  FramePtr after; CHECK(r.frame("main", after)); CHECK(after->px != b0.px);
}

TEST(media_failure_shows_placeholder_not_crash) {
  Rig r; show::ShowDocument d = testDoc();
  show::MediaRecord m; m.hash = std::string(64, 'a'); m.relPath = "media/missing.png"; m.format = "png"; d.media.push_back(m);
  show::Cue c; c.id = "c1"; c.sceneId = "s0"; c.mediaRef = m.hash; c.duration.ms = 0; d.cues.push_back(c);
  auto f = r.ctl->applyDocument(d); r.ctl->pump(); CHECK(f.get().ok());
  r.assignMain("B"); r.ctl->setMediaRoot(r.dir.path("nomedia"));
  CHECK(r.ctl->missingMedia().size() == 1);
  r.act(ActionId::ModeProgrammed); r.act(ActionId::TriggerCue, 0); r.run(500);
  FramePtr fr; CHECK(r.frame("main", fr)); CHECK(Rig::anyNonZero(*fr));
}
int main() { return ss_test::runAll(); }
