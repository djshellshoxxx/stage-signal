#include "test_show_util.h"
using namespace sst;

// ---- REQ-S01: autonomous selection ----------------------------------------------------------------
TEST(show_seeded_sequence_reproducible) {
  ShowDocument d = baseDoc(6); d.autoCfg.seed = 42;
  Rig a(d), b(d);
  a.tick(120000); b.tick(120000);
  CHECK(a.scenes().size() > 20);
  CHECK(a.scenes() == b.scenes());
  ShowDocument d2 = d; d2.autoCfg.seed = 43;
  Rig c(d2); c.tick(120000);
  CHECK(c.scenes() != a.scenes());
  // reseed reproduces from the start
  Rig r1(d), r2(d); r1.tick(30000); r2.tick(30000);
  CHECK(r1.e.snapshot().seed == 42);
}

TEST(show_repeat_history_pool_three) {
  ShowDocument d = baseDoc(5); d.autoCfg.repeatHistory = 2;
  for (int i = 0; i < 3; ++i) d.autoCfg.pool.push_back({"s" + std::to_string(i), 1});
  Rig r(d); r.tick(200000);
  auto v = r.scenes();
  CHECK(v.size() > 30);
  // initial scene s0 is on air at start: sequence must never repeat any of the last 2 shown
  std::vector<std::string> seen = {"s0"};
  for (const auto& s : v) {
    CHECK(s != seen.back());
    if (seen.size() >= 2) CHECK(s != seen[seen.size() - 2]);
    seen.push_back(s);
  }
  for (const auto& s : v) CHECK(s == "s0" || s == "s1" || s == "s2");   // pool eligibility
}

TEST(show_pool_size_one_and_two_edges) {
  ShowDocument d = baseDoc(4); d.autoCfg.pool = {{"s0", 5}};
  Rig one(d); one.tick(60000);
  CHECK(one.scenes().empty());                       // single scene: stays, no thrash, no repeat events
  CHECK(one.e.snapshot().sceneIndex == 0);
  ShowDocument d2 = baseDoc(4); d2.autoCfg.pool = {{"s0", 1}, {"s1", 1}}; d2.autoCfg.repeatHistory = 5;   // history > pool: capped
  Rig two(d2); two.tick(60000);
  auto v = two.scenes();
  CHECK(v.size() > 10);
  for (size_t i = 0; i < v.size(); ++i) CHECK(v[i] == (i % 2 == 0 ? "s1" : "s0"));   // strict alternation
  // pool of one that is not on air: moves to it once
  ShowDocument d3 = baseDoc(4); d3.autoCfg.pool = {{"s2", 1}};
  Rig three(d3); three.tick(30000);
  CHECK(three.scenes().size() == 1 && three.scenes()[0] == "s2");
}

TEST(show_zero_weight_and_min_dwell) {
  ShowDocument d = baseDoc(4); d.autoCfg.pool = {{"s0", 1}, {"s1", 0}, {"s2", 3}, {"s3", 1}};
  d.autoCfg.minDwellMs = 2000; d.autoCfg.maxDwellMs = 2000; d.autoCfg.repeatHistory = 0;
  Rig r(d);
  int64_t last = r.clk.t; int changes = 0;
  for (int i = 0; i < 2000; ++i) {
    int before = (int)r.scenes().size();
    r.tick(10);
    if ((int)r.scenes().size() != before) { CHECK(r.clk.t - last >= 2000 * kNsPerMs - 10 * kNsPerMs); last = r.clk.t; ++changes; }
  }
  CHECK(changes >= 9);
  for (const auto& s : r.scenes()) CHECK(s != "s1");
}

// ---- REQ-S03/S04: override, pause, resume -------------------------------------------------------------
static ShowDocument cueDoc() {
  ShowDocument d = baseDoc(4); d.defaultMode = 1;
  d.cues = {msCue("c0", "s0", 1000), msCue("c1", "s1", 1000), msCue("c2", "s2", 1000), msCue("c3", "s3", 0)};
  return d;
}

TEST(show_programmed_ordering_and_timing) {
  Rig r(cueDoc());
  r.tick(5000);
  std::vector<std::string> order;
  for (const auto& j : r.journal) if (j.first == "cue_start") { size_t p = j.second.find("\"cue\":\""); order.push_back(j.second.substr(p + 7, 2)); }
  CHECK(order.size() == 4 && order[0] == "c0" && order[1] == "c1" && order[2] == "c2" && order[3] == "c3");
  auto s = r.e.snapshot();
  CHECK(s.cueIndex == 3);                 // last cue is a hold (duration 0) -> free time
  CHECK(r.e.snapshot().sceneIndex == 3);
  r.tick(60000);
  CHECK(r.e.snapshot().cueIndex == 3);    // holds until operator advances
  CHECK(r.count("cue_start") == 4);
}

TEST(show_programmed_timing_exact_regardless_of_tick) {
  ShowDocument d = cueDoc();
  FeatureSnapshot f; f.sequence = 1;
  ShowEngine a, b; CHECK(a.replaceDocument(d).ok()); CHECK(b.replaceDocument(d).ok());
  a.update(0, f); b.update(0, f);
  for (int i = 1; i <= 2500; ++i) { f.sequence++; a.update((Ns)i * kNsPerMs, f); }       // 1 ms ticks
  for (int i = 1; i <= 25; ++i) { f.sequence++; b.update((Ns)i * 100 * kNsPerMs, f); }  // 100 ms ticks
  CHECK(a.snapshot().cueIndex == 2 && b.snapshot().cueIndex == 2);
  CHECK_NEAR(a.snapshot().cueElapsedSec, b.snapshot().cueElapsedSec, 1e-6);
  CHECK_NEAR(a.snapshot().cueElapsedSec, 0.5, 1e-6);
}

TEST(show_manual_override_blocks_auto_cues) {
  Rig r(cueDoc());
  r.tick(500);
  CHECK(r.e.snapshot().cueIndex == 0);
  ActionResult ar = r.go(act(ActionId::SelectScene, 3));
  CHECK(ar.accepted);
  auto s = r.e.snapshot();
  CHECK(s.manualOverride && s.sceneIndex == 3 && s.cueIndex == 0);
  int starts = r.count("cue_start");
  r.tick(30000);                                                    // no cue may fire while overridden
  CHECK(r.count("cue_start") == starts);
  CHECK(r.e.snapshot().cueIndex == 0 && r.e.snapshot().sceneIndex == 3);
  CHECK_NEAR(r.e.snapshot().cueElapsedSec, 0.5, 1e-9);              // position preserved
  CHECK(r.go(act(ActionId::ResumeContinue)).accepted);
  CHECK(!r.e.snapshot().manualOverride);
  CHECK(r.e.snapshot().sceneIndex == 3);                            // no surprise jump
  r.tick(400); CHECK(r.e.snapshot().cueIndex == 0);
  r.tick(200); CHECK(r.e.snapshot().cueIndex == 1);                 // remaining 0.5 s continued
}

TEST(show_auto_manual_select_becomes_hybrid_and_resume_restores) {
  ShowDocument d = baseDoc(4); d.autoCfg.pool = {{"s0", 1}, {"s1", 1}, {"s2", 1}, {"s3", 1}};
  Rig r(d);
  r.tick(500);
  CHECK(r.go(act(ActionId::SelectScene, 2)).accepted);
  auto s = r.e.snapshot();
  CHECK(s.mode == ShowMode::Hybrid && s.manualOverride && s.sceneIndex == 2);
  size_t n = r.scenes().size();
  r.tick(100000);
  CHECK(r.scenes().size() == n && r.e.snapshot().sceneIndex == 2);   // no auto change during override
  r.go(act(ActionId::ResumeAutomation));
  CHECK(r.e.snapshot().mode == ShowMode::Auto && !r.e.snapshot().manualOverride);
  r.tick(10000);
  CHECK(r.scenes().size() > n);
  // Show-originated scene selection does not override
  Rig q(d); q.go(act(ActionId::SelectScene, 1, ActionSource::Show));
  CHECK(!q.e.snapshot().manualOverride && q.e.snapshot().mode == ShowMode::Auto);
}

TEST(show_pause_resume_cue_position_and_next) {
  Rig r(cueDoc());
  r.tick(600);
  r.go(act(ActionId::PauseAutomation));
  CHECK(r.e.snapshot().paused);
  r.tick(20000);
  CHECK(r.e.snapshot().cueIndex == 0);
  CHECK_NEAR(r.e.snapshot().cueElapsedSec, 0.6, 1e-9);
  r.go(act(ActionId::ResumeContinue));
  r.tick(300); CHECK(r.e.snapshot().cueIndex == 0);
  r.tick(150); CHECK(r.e.snapshot().cueIndex == 1);
  // resume with next-cue progression
  r.go(act(ActionId::PauseAutomation)); r.tick(5000);
  CHECK(r.e.snapshot().cueIndex == 1);
  CHECK(r.go(act(ActionId::ResumeNext)).accepted);
  CHECK(r.e.snapshot().cueIndex == 2 && r.e.snapshot().sceneIndex == 2 && !r.e.snapshot().paused);
  // idempotent pause
  uint64_t rev = r.e.snapshot().revision;
  r.go(act(ActionId::PauseAutomation)); uint64_t rev2 = r.e.snapshot().revision;
  ActionResult again = r.go(act(ActionId::PauseAutomation));
  CHECK(again.accepted && r.e.snapshot().revision == rev2 && rev2 > rev);
}

TEST(show_cue_navigation_and_modes) {
  Rig r(cueDoc());
  r.tick(100);
  CHECK(r.go(act(ActionId::NextCue)).accepted && r.e.snapshot().cueIndex == 1);
  CHECK(r.go(act(ActionId::PrevCue)).accepted && r.e.snapshot().cueIndex == 0);
  CHECK(!r.go(act(ActionId::PrevCue)).accepted);
  CHECK(r.go(act(ActionId::TriggerCue, 3)).accepted && r.e.snapshot().cueIndex == 3);
  CHECK(!r.go(act(ActionId::NextCue)).accepted);                    // last, Hold
  CHECK(!r.go(act(ActionId::TriggerCue, 9)).accepted);
  CHECK(!r.go(act(ActionId::TriggerCue, 1.5)).accepted);
  r.go(act(ActionId::ModeAuto));
  CHECK(!r.go(act(ActionId::NextCue)).accepted);                    // cues need Programmed/Hybrid
  ShowDocument noCue = baseDoc(2); Rig n(noCue);
  CHECK(!n.go(act(ActionId::ModeProgrammed)).accepted);
}

TEST(show_cue_loop_and_hybrid_release) {
  ShowDocument d = baseDoc(3); d.defaultMode = 1; d.cueEnd = CueListEnd::Loop;
  d.cues = {msCue("a", "s0", 500), msCue("b", "s1", 500)};
  Rig r(d); r.tick(3100);
  CHECK(r.count("cue_start") >= 6);
  ShowDocument h = baseDoc(3); h.defaultMode = 2; h.cues = {msCue("a", "s1", 500)};
  Rig q(h); q.go(act(ActionId::TriggerCue, 0));
  CHECK(q.e.snapshot().cueIndex == 0);
  q.tick(700);
  CHECK(q.e.snapshot().cueIndex == -1);                              // hybrid: cue releases back to autonomous
}

TEST(show_beat_cues_use_effective_bpm_and_never_assume) {
  ShowDocument d = baseDoc(3); d.defaultMode = 1;
  d.cues = {beatCue("b0", "s0", 4), beatCue("b1", "s1", 4)};
  Rig r(d);
  r.tick(5000);
  CHECK(r.e.snapshot().cueIndex == 0 && r.e.snapshot().cueWaitingForTempo);   // unknown tempo: waits
  r.go(act(ActionId::SetBpm, 120));
  r.tick(1900); CHECK(r.e.snapshot().cueIndex == 0 && !r.e.snapshot().cueWaitingForTempo);   // 4 beats @120 = 2 s
  r.tick(200);  CHECK(r.e.snapshot().cueIndex == 1);
  // estimated tempo is used when no override
  Rig q(d); q.f.tempo.valid = true; q.f.tempo.value = 240; q.f.tempo.confidence = 0.9f;
  q.tick(900); CHECK(q.e.snapshot().cueIndex == 0);
  q.tick(200); CHECK(q.e.snapshot().cueIndex == 1);
}

TEST(show_quantized_triggers) {
  ShowDocument d = cueDoc(); d.defaultMode = 2;
  Rig r(d);
  CHECK(!r.e.submit([] { Action a = act(ActionId::TriggerCue, 1); a.trigger = Trigger::Beat; return a; }(), r.clk.t).accepted);   // tempo unknown
  r.go(act(ActionId::SetBpm, 120));                                  // beat grid anchored now, 500 ms period
  r.tick(120);
  Action a = act(ActionId::TriggerCue, 2); a.trigger = Trigger::Beat;
  ActionResult ar = r.go(a); CHECK(ar.accepted && ar.reason == "scheduled");
  CHECK(r.e.snapshot().cueIndex == -1);
  r.tick(300); CHECK(r.e.snapshot().cueIndex == -1);
  r.tick(100); CHECK(r.e.snapshot().cueIndex == 2);                  // next beat boundary at 500 ms after anchor
  Action b = act(ActionId::TriggerCue, 1); b.trigger = Trigger::Bar;
  r.go(act(ActionId::SetBpm, 120)); r.tick(10);
  CHECK(r.go(b).accepted);
  r.tick(1400); CHECK(r.e.snapshot().cueIndex == -1);               // earlier hybrid cue released; bar not reached
  r.tick(600);  CHECK(r.e.snapshot().cueIndex == 1);                 // bar = 4 beats = 2 s
  Action t = act(ActionId::TriggerCue, 0); t.trigger = Trigger::Time; t.when = r.clk.t + 700 * kNsPerMs;
  CHECK(r.go(t).accepted); r.tick(600); CHECK(r.e.snapshot().cueIndex != 0); r.tick(200); CHECK(r.e.snapshot().cueIndex == 0);
}

TEST(show_quantized_cue_start_on_beat) {
  ShowDocument d = baseDoc(3); d.defaultMode = 1;
  d.cues = {msCue("a", "s0", 730), msCue("b", "s1", 1000, Quantize::Beat)};
  Rig r(d); r.go(act(ActionId::SetBpm, 120)); r.tick(10);
  r.tick(800);                                                        // cue a ended at ~730 ms, b waits for next 500 ms grid line
  auto s = r.e.snapshot(); CHECK(s.cueIndex == 0 || s.cueIndex == 1);
  r.tick(300);
  CHECK(r.e.snapshot().cueIndex == 1);
}

TEST(show_missing_cue_handling) {
  ShowDocument d = cueDoc();
  d.cues[1].sceneId = "ghost"; d.cues[2].mediaRef = "ab";
  ShowEngine e; Status bad = e.replaceDocument(d); CHECK(!bad.ok());                 // validator rejects dangling refs
  std::vector<std::string> kinds; e.setJournalHook([&](const std::string& k, const std::string&) { kinds.push_back(k); });
  e.adoptUnvalidated(d);
  FeatureSnapshot f; Ns t = 0;
  for (int i = 0; i < 150; ++i) { t += 10 * kNsPerMs; f.sequence++; e.update(t, f); }
  auto s = e.snapshot();
  CHECK(s.cueIndex == 2);                                              // c1 skipped, never stalls
  CHECK(s.cueMediaMissing);                                            // media missing flagged, scene still runs
  int skipped = 0; for (auto& k : kinds) if (k == "cue_skipped") ++skipped;
  CHECK(skipped == 1);
  CHECK(!e.submit([] { Action a; a.id = ActionId::TriggerCue; a.value = 1; return a; }(), t).accepted);   // explicit trigger of missing cue refused
}

// ---- blackout (REQ-D08, §11) --------------------------------------------------------------------------------
TEST(show_blackout_arbitration_and_restore) {
  Rig r(cueDoc());
  r.tick(300);
  r.go(act(ActionId::VisualBlackout));
  auto s = r.e.snapshot();
  CHECK(s.visualBlackoutLatch && s.visualBlackedOut && !s.lightingBlackedOut && !s.paused);   // separate from pause and lighting
  r.tick(2000);                                                       // show keeps running under blackout
  CHECK(r.e.snapshot().cueIndex == 2 && r.e.snapshot().visualBlackedOut);
  r.go(act(ActionId::LightingBlackout));
  r.go(act(ActionId::EmergencyBlackout));
  s = r.e.snapshot();
  CHECK(s.visualBlackedOut && s.lightingBlackedOut && s.emergencyBlackoutLatch);
  Action rs = act(ActionId::RestoreBlackout, 3);                     // emergency only
  r.go(rs);
  s = r.e.snapshot();
  CHECK(!s.emergencyBlackoutLatch && s.visualBlackedOut && s.lightingBlackedOut);   // individual latches still hold
  r.go(act(ActionId::RestoreBlackout, 1));
  s = r.e.snapshot(); CHECK(!s.visualBlackedOut && s.lightingBlackedOut);
  r.go(act(ActionId::RestoreBlackout, 0));
  s = r.e.snapshot(); CHECK(!s.visualBlackedOut && !s.lightingBlackedOut);
  CHECK(s.sceneIndex == 2 && s.cueIndex == 2);                        // prior scene state retained
  CHECK(!r.go(act(ActionId::RestoreBlackout, 7)).accepted);
  // pause does not engage blackout and vice versa
  r.go(act(ActionId::PauseAutomation)); CHECK(!r.e.snapshot().visualBlackedOut);
  // idempotent engage + never refused for stale revision
  r.go(act(ActionId::EmergencyBlackout));
  Action stale = act(ActionId::VisualBlackout); stale.expectRevision = 1;
  CHECK(r.go(stale).accepted);
  // survives document replacement (safety)
  CHECK(r.e.replaceDocument(cueDoc()).ok());
  CHECK(r.e.snapshot().emergencyBlackoutLatch && r.e.snapshot().visualBlackedOut);
  CHECK(!r.e.snapshot().lightingArmRequested);
}

TEST(show_blackout_ignores_triggers_and_applies_immediately) {
  Rig r(cueDoc());
  Action a = act(ActionId::EmergencyBlackout); a.trigger = Trigger::Bar; a.when = r.clk.t + 10 * kNsPerSec;
  CHECK(r.go(a).accepted);
  CHECK(r.e.snapshot().emergencyBlackoutLatch && r.e.snapshot().pendingActions == 0);
}

// ---- REQ-S12 strobe ----------------------------------------------------------------------------------------------
TEST(show_disable_strobing) {
  ShowDocument d = baseDoc(4); makeStrobe(d, 1); makeStrobe(d, 3);
  d.cues = {msCue("c0", "s0", 500), msCue("c1", "s1", 500), msCue("c2", "s2", 500)}; d.defaultMode = 2;
  d.midiMappings = {"{\"action\":\"flash\",\"strobe\":true}", "{\"action\":\"next\"}"};
  Rig r(d);
  CHECK(r.e.mappingAllowed(d.midiMappings[0]));                       // allowed until disabled
  r.go(act(ActionId::SelectScene, 1)); CHECK(r.e.snapshot().sceneIndex == 1 && !r.e.snapshot().sceneStrobeSuppressed);
  r.go(act(ActionId::DisableStrobing, 1));
  auto s = r.e.snapshot();
  CHECK(s.strobeDisabled && s.sceneIndex != 1 && s.sceneIndex != 3);  // on-air strobe scene replaced
  CHECK(!r.go(act(ActionId::SelectScene, 3)).accepted);
  CHECK(!r.go(act(ActionId::TriggerCue, 1)).accepted);                // cue pointing to strobe scene refused
  CHECK(!r.e.mappingAllowed(d.midiMappings[0]) && r.e.mappingAllowed(d.midiMappings[1]));
  for (int i = 0; i < 8; ++i) { r.go(act(ActionId::NextScene)); CHECK(r.e.snapshot().sceneIndex == 0 || r.e.snapshot().sceneIndex == 2); }
  r.go(act(ActionId::ResumeAutomation));
  size_t base = r.scenes().size();
  r.tick(100000);
  CHECK(r.scenes().size() > base + 10);
  for (size_t i = base; i < r.scenes().size(); ++i) CHECK(r.scenes()[i] != "s1" && r.scenes()[i] != "s3");  // autonomous pool excludes strobe scenes
  CHECK(!r.go(act(ActionId::DisableStrobing, 0.5)).accepted);
  // persisted in the document
  CHECK(r.e.documentWithState().disableStrobing);
  r.go(act(ActionId::DisableStrobing, 0)); CHECK(!r.e.snapshot().strobeDisabled && r.go(act(ActionId::SelectScene, 3)).accepted);
  // all scenes strobe -> suppression flag instead of crash
  ShowDocument all = baseDoc(2); makeStrobe(all, 0); makeStrobe(all, 1);
  Rig q(all); q.go(act(ActionId::DisableStrobing, 1));
  CHECK(q.e.snapshot().sceneStrobeSuppressed);
  CHECK(!q.go(act(ActionId::NextScene)).accepted);
}

// ---- REQ-A06/A08 tempo & key -----------------------------------------------------------------------------------
TEST(show_tap_tempo_deterministic) {
  Rig r(baseDoc(2));
  Ns t0 = r.clk.t;
  auto tap = [&](Ns t) { Action a = act(ActionId::TapTempo); a.when = t; return r.e.submit(a, t); };
  CHECK(tap(t0 + 100 * kNsPerMs).accepted);
  CHECK(!r.e.snapshot().tempoOverridden);                             // one tap is not a tempo
  CHECK(tap(t0 + 600 * kNsPerMs).accepted);
  auto s = r.e.snapshot(); CHECK(s.tempoOverridden && s.tempo.source == Estimate::Tap); CHECK_NEAR(s.tempo.value, 120.0, 1e-9);
  for (int i = 2; i < 6; ++i) tap(t0 + 100 * kNsPerMs + (Ns)i * 500 * kNsPerMs);
  CHECK_NEAR(r.e.snapshot().tempo.value, 120.0, 1e-9);
  // sliding window of last 8: switch to 100 BPM (600 ms), after enough taps the average follows
  Ns t = t0 + 100 * kNsPerMs + 5 * 500 * kNsPerMs;
  for (int i = 0; i < 8; ++i) { t += 600 * kNsPerMs; tap(t); }
  CHECK_NEAR(r.e.snapshot().tempo.value, 100.0, 1e-9);
  CHECK(!tap(t + 100 * kNsPerMs).accepted);                           // 100 ms = bounce, rejected
  CHECK(!tap(t - 1).accepted);                                        // non-increasing timestamp
  // reset gap: a long pause restarts the sequence; old average is dropped
  Ns u = t + 5 * kNsPerSec;
  tap(u); CHECK_NEAR(r.e.snapshot().tempo.value, 100.0, 1e-9);        // single tap keeps previous override
  tap(u + 400 * kNsPerMs); CHECK_NEAR(r.e.snapshot().tempo.value, 150.0, 1e-9);
  // same inputs -> same result (determinism)
  Rig r2(baseDoc(2));
  auto tap2 = [&](Ns t) { Action a = act(ActionId::TapTempo); a.when = t; return r2.e.submit(a, t); };
  tap2(1000 * kNsPerMs); tap2(1400 * kNsPerMs); tap2(1800 * kNsPerMs);
  CHECK_NEAR(r2.e.snapshot().tempo.value, 150.0, 1e-9);
  // too slow interval (<30 BPM) rejected
  Rig r3(baseDoc(2));
  Action a1 = act(ActionId::TapTempo); a1.when = 1 * kNsPerSec; r3.e.submit(a1, a1.when);
  Action a2 = act(ActionId::TapTempo); a2.when = 3400 * kNsPerMs; CHECK(!r3.e.submit(a2, a2.when).accepted);
}

TEST(show_manual_bpm_key_overrides_until_released) {
  ShowDocument d = baseDoc(2);
  Rig r(d);
  r.f.tempo.valid = true; r.f.tempo.value = 128; r.f.tempo.confidence = 0.8f; r.f.key.valid = true; r.f.key.value = 5; r.f.key.confidence = 0.7f;
  r.tick(100);
  auto s = r.e.snapshot();
  CHECK(s.tempo.valid && s.tempo.source == Estimate::Estimated && s.tempo.value == 128 && !s.tempoOverridden);
  CHECK(!r.go(act(ActionId::SetBpm, 5)).accepted && !r.go(act(ActionId::SetBpm, 500)).accepted);
  CHECK(r.go(act(ActionId::SetBpm, 90)).accepted);
  CHECK(r.go(act(ActionId::SetKey, 14)).accepted);                    // D minor (2 + 12)
  r.tick(5000);                                                       // estimator keeps changing; override sticks
  r.f.tempo.value = 140; r.tick(100);
  s = r.e.snapshot();
  CHECK(s.tempo.source == Estimate::Manual && s.tempo.value == 90 && s.tempo.confidence == 1.0f);
  CHECK(s.key.source == Estimate::Manual && s.key.value == 2 && s.key.minor);
  FeatureSnapshot eff = r.e.effectiveFeatures();
  CHECK(eff.tempo.value == 90 && eff.key.value == 2 && eff.key.minor && eff.rmsDbMix == r.f.rmsDbMix);
  CHECK(!r.go(act(ActionId::SetKey, 24)).accepted && !r.go(act(ActionId::SetKey, 1.5)).accepted);
  // overrides are saved with the document and restored on load (REQ-A08)
  ShowDocument saved = r.e.documentWithState();
  CHECK(saved.manualBpm == 90 && saved.manualKey == 14);
  Rig r2(saved); r2.tick(50);
  CHECK(r2.e.snapshot().tempo.value == 90 && r2.e.snapshot().key.minor && r2.e.snapshot().key.source == Estimate::Manual);
  r.go(act(ActionId::ReleaseBpm)); r.go(act(ActionId::ReleaseKey)); r.tick(50);
  s = r.e.snapshot();
  CHECK(s.tempo.source == Estimate::Estimated && s.tempo.value == 140 && s.key.source == Estimate::Estimated && s.key.value == 5);
  CHECK(r.e.documentWithState().manualBpm == 0 && r.e.documentWithState().manualKey == -1);
  // unknown estimate stays Unknown (not zero)
  r.f.tempo.valid = false; r.tick(20); CHECK(!r.e.snapshot().tempo.valid);
}

// ---- REQ-S05 text -------------------------------------------------------------------------------------------------------
TEST(show_text_draft_preview_publish_separation) {
  ShowDocument d = baseDoc(2); d.texts = {{"welcome", "Welcome \xE2\x98\x83!"}, {"bye", "Goodnight"}};
  Rig r(d);
  CHECK(r.e.setDraft("Hello band").ok());
  auto s = r.e.snapshot();
  CHECK(s.draftText == "Hello band" && s.outputs[0].publishedText.empty() && s.outputs[1].publishedText.empty());
  CHECK(!r.e.publishDraft().ok());                                    // must preview first
  CHECK(r.e.previewDraft().ok());
  s = r.e.snapshot(); CHECK(s.previewText == "Hello band" && s.outputs[0].publishedText.empty());
  CHECK(r.e.setDraft("Hello band!").ok());                            // editing after preview invalidates it
  CHECK(!r.e.publishDraft().ok());
  r.e.previewDraft();
  CHECK(r.e.publishDraft(0).ok());                                    // publish to output 0 only
  s = r.e.snapshot(); CHECK(s.outputs[0].publishedText == "Hello band!" && s.outputs[1].publishedText.empty() && s.outputs[2].publishedText.empty());
  // a scene change / any other action never publishes
  r.go(act(ActionId::NextScene)); CHECK(r.e.snapshot().outputs[1].publishedText.empty());
  CHECK(r.go(act(ActionId::PublishText, 0)).accepted);                // all visual outputs, never the controls output
  s = r.e.snapshot(); CHECK(s.outputs[0].publishedTextId == "welcome" && s.outputs[1].publishedText == "Welcome \xE2\x98\x83!" && s.outputs[2].publishedText.empty());
  CHECK(!r.go(act(ActionId::PublishText, 5)).accepted);
  r.go(act(ActionId::ClearText)); CHECK(r.e.snapshot().outputs[0].publishedText.empty());
  // limits: 4096 code points (multi-byte counted as one), invalid UTF-8 rejected
  std::string ok; for (int i = 0; i < 4096; ++i) ok += "\xC3\xA9";
  CHECK(r.e.setDraft(ok).ok());
  CHECK(!r.e.setDraft(ok + "x").ok());
  CHECK(!r.e.setDraft("bad\xC0\xAF").ok());
  CHECK(!r.e.setDraft(std::string("a\0b", 3)).ok());
  CHECK(r.count("text_publish") == 2);
}

// ---- REQ-D09 idle / audio loss ---------------------------------------------------------------------------------------------
TEST(show_audio_loss_policies_within_two_seconds) {
  ShowDocument cont = baseDoc(3); cont.idle.audioLoss = AudioLossPolicy::Continue;
  Rig a(cont); a.tick(500);
  Ns t0 = a.clk.t;
  a.tick(2000, false);                                                // device gone: no new feature sequences
  CHECK(a.e.snapshot().audioLost && !a.e.snapshot().idleActive);      // continue policy keeps visuals running
  ShowDocument idle = baseDoc(3); idle.idle.audioLoss = AudioLossPolicy::Idle; idle.idle.screen = "logo";
  Rig b(idle); b.tick(500);
  t0 = b.clk.t;
  Ns detected = -1;
  for (int i = 0; i < 300 && detected < 0; ++i) { b.tick(10, false); if (b.e.snapshot().idleActive) detected = b.clk.t - t0; }
  CHECK(detected > 0 && detected <= 2 * kNsPerSec);
  auto s = b.e.snapshot(); CHECK(s.idleActive && s.audioLost && s.idleScreen == "logo");
  size_t n = b.scenes().size(); b.tick(30000, false); CHECK(b.scenes().size() == n);   // automation frozen while idle
  b.go(act(ActionId::SelectScene, 1)); CHECK(b.e.snapshot().idleActive);                 // audio-loss idle not dismissed by operator
  b.tick(100); CHECK(!b.e.snapshot().audioLost && !b.e.snapshot().idleActive);          // audio back -> idle clears
  // explicit device notification is immediate
  Rig c(idle); c.tick(100); c.e.setAudioAvailable(false); CHECK(c.e.snapshot().idleActive); c.e.setAudioAvailable(true); c.tick(20); CHECK(!c.e.snapshot().idleActive);
  // silence is not loss
  Rig d(idle); d.f.silent = true; d.tick(5000); CHECK(!d.e.snapshot().audioLost);
  // recovery start: idle until operator acts, lighting disarmed
  EngineOptions o; o.startIdle = true; Rig e(baseDoc(2), o);
  CHECK(e.e.snapshot().idleActive && !e.e.snapshot().lightingArmRequested);
  e.go(act(ActionId::SelectScene, 1)); CHECK(!e.e.snapshot().idleActive);
}

// ---- outputs / actions / revisions ----------------------------------------------------------------------------------------------
TEST(show_output_targets_and_ranges) {
  Rig r(baseDoc(4));
  CHECK(r.go(act(ActionId::Brightness, 0.4)).accepted);
  auto s = r.e.snapshot(); CHECK(s.outputs[0].brightness == 0.4f && s.outputs[1].brightness == 0.4f && s.outputs[2].brightness == 1.0f);   // controls untouched
  CHECK(r.go(act(ActionId::SetTarget, 1)).accepted);
  CHECK(r.e.snapshot().target == 1 && r.e.snapshot().outputs[1].selected);
  CHECK(r.go(act(ActionId::Speed, 2)).accepted);
  s = r.e.snapshot(); CHECK(s.outputs[1].speed == 2.0f && s.outputs[0].speed == 1.0f);
  Action direct = act(ActionId::ColorHue, 0.25); direct.target = 0; CHECK(r.go(direct).accepted);
  CHECK(r.e.snapshot().outputs[0].hue == 0.25f);
  Action ctl = act(ActionId::Intensity, 0.5); ctl.target = 2; CHECK(!r.go(ctl).accepted);
  Action oob = act(ActionId::Intensity, 0.5); oob.target = 9; CHECK(!r.go(oob).accepted);
  CHECK(!r.go(act(ActionId::Speed, 0.1)).accepted && !r.go(act(ActionId::Speed, 9)).accepted && !r.go(act(ActionId::Brightness, -0.1)).accepted);
  CHECK(!r.go(act(ActionId::Brightness, 1.0 / 0.0)).accepted);
  CHECK(!r.go(act(ActionId::SetTarget, 2)).accepted && !r.go(act(ActionId::SetTarget, 3)).accepted);
  // independent scene per output; shared selection with -1
  r.go(act(ActionId::SelectScene, 2));                                // target 1 selected -> only output 1
  s = r.e.snapshot(); CHECK(s.outputs[1].sceneIndex == 2 && s.outputs[1].independentScene && s.outputs[0].sceneIndex == s.sceneIndex);
  r.go(act(ActionId::SetTarget, -1)); r.go(act(ActionId::SelectScene, 3));
  s = r.e.snapshot(); CHECK(s.sceneIndex == 3 && !s.outputs[1].independentScene && s.outputs[1].sceneIndex == 3);
  CHECK(r.go(act(ActionId::SelectBank, 2)).accepted && r.e.snapshot().bank == 2);
  CHECK(!r.go(act(ActionId::StartRecording)).accepted);               // not a show action
  CHECK(!r.go(act(ActionId::SelectScene, 99)).accepted);
}

TEST(show_transition_progress_and_outgoing) {
  ShowDocument d = baseDoc(3); d.autoCfg.transition = {"fade", 1000};
  Rig r(d);
  r.go(act(ActionId::SelectScene, 2));
  auto s = r.e.snapshot(); CHECK(s.sceneIndex == 2 && s.outgoingSceneIndex == 0 && s.transitionProgress == 0.0f && s.transitionType == "fade");
  r.tick(500); s = r.e.snapshot(); CHECK_NEAR(s.transitionProgress, 0.5, 0.03); CHECK(s.outgoingSceneIndex == 0);
  r.tick(600); s = r.e.snapshot(); CHECK(s.transitionProgress == 1.0f && s.outgoingSceneIndex == -1);
}

TEST(show_revision_monotonic_and_expectation) {
  Rig r(baseDoc(3));
  uint64_t last = r.e.snapshot().revision;
  for (int i = 0; i < 50; ++i) {
    r.go(act(i % 2 ? ActionId::NextScene : ActionId::Brightness, i % 2 ? 0 : (i % 10) / 10.0));
    r.tick(20);
    uint64_t now = r.e.snapshot().revision; CHECK(now >= last); last = now;
  }
  uint64_t rev = r.e.snapshot().revision;
  r.tick(30); CHECK(r.e.snapshot().revision == rev || r.e.snapshot().revision > rev);
  Action a = act(ActionId::SelectBank, 5); a.expectRevision = rev + 1000;
  ActionResult ar = r.go(a); CHECK(!ar.accepted && ar.reason.find("stale") == 0);
  a.expectRevision = r.e.snapshot().revision; ActionResult ok = r.go(a); CHECK(ok.accepted && ok.revision > a.expectRevision);
  uint64_t cur = r.e.snapshot().revision;
  a.expectRevision = 0; r.go(a);                                      // idempotent re-submit: no change, no revision bump
  CHECK(r.e.snapshot().revision == cur);
  // snapshot is an immutable copy
  ShowSnapshot s1 = r.e.snapshot(); r.go(act(ActionId::SelectBank, 7)); CHECK(s1.bank == 5 && r.e.snapshot().bank == 7);
}

TEST(show_lighting_arm_request_defaults_disarmed) {
  Rig r(baseDoc(2));
  CHECK(!r.e.snapshot().lightingArmRequested);
  r.go(act(ActionId::ArmLighting)); CHECK(r.e.snapshot().lightingArmRequested);
  r.e.replaceDocument(baseDoc(2)); CHECK(!r.e.snapshot().lightingArmRequested);   // loading never arms
  r.go(act(ActionId::ArmLighting)); r.go(act(ActionId::DisarmLighting)); CHECK(!r.e.snapshot().lightingArmRequested);
}

TEST(show_invalid_document_leaves_live_show_active) {
  Rig r(baseDoc(3));
  r.go(act(ActionId::SelectScene, 2)); r.go(act(ActionId::Brightness, 0.3));
  ShowSnapshot before = r.e.snapshot();
  ShowDocument bad = baseDoc(3); bad.scenes[1].rendererType = "does_not_exist";
  Status s = r.e.replaceDocument(bad);
  CHECK(!s.ok() && s.code == Err::InvalidArgument);
  ShowSnapshot after = r.e.snapshot();
  CHECK(after.revision == before.revision && after.sceneIndex == 2 && after.outputs[0].brightness == 0.3f);
  CHECK(r.e.document().scenes[1].rendererType != "does_not_exist");
  bad = baseDoc(3); bad.schemaMajor = 2; CHECK(r.e.replaceDocument(bad).code == Err::SchemaUnsupported);
}

int main() { return ss_test::runAll(); }
