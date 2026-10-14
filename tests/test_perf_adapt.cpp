#include "test.h"
#include "perf/adaptation.h"
#include <functional>
using namespace ss;

// Drives the controller at 100 ms ticks with a load model; records the time (seconds) of each level change.
struct Sim {
  AdaptationController c; Ns t = 0; uint64_t eval = 0, miss = 0;
  std::vector<std::pair<double, int>> changes;
  explicit Sim(AdaptationConfig cfg = AdaptationConfig()) : c(cfg) {}
  // cpu in %, missFrac 0..1, enc/max queue fraction 0..1
  void run(double seconds, double cpu, double missFrac = 0, double enc = 0, double maxq = -1, bool cpuAvail = true) {
    Ns end = t + (Ns)(seconds * 1e9);
    while (t < end) {
      eval += 10; miss += (uint64_t)(10 * missFrac + 1e-9);
      PressureSample s; s.now = t; s.cpuAvailable = cpuAvail; s.processCpuPct = cpu; s.systemCpuPct = cpu - 5; s.deadlinesEvaluated = eval; s.deadlinesMissed = miss;
      s.encoderQueueFraction = enc; s.maxQueueFraction = maxq < 0 ? enc : maxq;
      int before = c.level(); c.update(s); if (c.level() != before) changes.push_back({nsToSec(t), c.level()});
      t += 100 * kNsPerMs;
    }
  }
};
static bool near(double a, double b) { return a > b - 0.06 && a < b + 0.06; }

TEST(adapt_ladder_sequence_under_cpu_load) {
  Sim s; s.run(60, 95);
  // one step per 5 s, first after 5 s sustained: t = 5,10,...,35 ; 7 steps total
  CHECK(s.changes.size() == 7);
  for (size_t i = 0; i < s.changes.size(); ++i) { CHECK(near(s.changes[i].first, 5.0 * (double)(i + 1))); CHECK(s.changes[i].second == (int)i + 1); }
  std::vector<AdaptStep> want = {AdaptStep::PreviewFps5, AdaptStep::TrailsParticles, AdaptStep::RenderScale, AdaptStep::SourceRequest, AdaptStep::SceneFps25, AdaptStep::SceneFps20, AdaptStep::SceneFps15};
  CHECK(s.c.ladder() == want);
  int sd = 0; AdaptStep last = AdaptStep::None; for (auto& e : s.c.events()) if (e.kind == AdaptEventKind::StepDown) { ++sd; last = e.step; } CHECK(sd == 7 && last == AdaptStep::SceneFps15);
  QualityState q = s.c.state();
  CHECK(q.previewFps == 5 && q.trailsParticlesReduced && q.renderScale == 0.75 && q.sourceRequestReduced && q.sceneFps == 15 && q.level == 7);
  CHECK(s.c.warnings() >= 1);                        // ladder exhausted warning, not silent
}
TEST(adapt_state_progression_per_level) {
  AdaptationController c; QualityState q;
  q = c.state(); CHECK(q.previewFps == 30 && !q.trailsParticlesReduced && q.renderScale == 1.0 && !q.sourceRequestReduced && q.sceneFps == 30);
  Sim s; double fps[] = {30, 30, 30, 30, 30, 25, 20, 15}; double prev[] = {30, 5, 5, 5, 5, 5, 5, 5};
  s.run(40, 95);
  AdaptationController d; d.update(PressureSample());
  for (int lvl = 0; lvl <= 7; ++lvl) { AdaptationController e; Sim ss; ss.run(lvl * 5.0 + 0.5, 95); QualityState x = ss.c.state(); CHECK(x.level == lvl); CHECK(x.sceneFps == fps[lvl] && x.previewFps == prev[lvl]); }
}
TEST(adapt_trigger_thresholds_exact) {
  { Sim s; s.run(20, 84.9); CHECK(s.changes.empty()); }                  // below 85 never triggers (process=84.9, system=79.9)
  { Sim s; s.run(4.8, 85); CHECK(s.changes.empty()); s.run(1, 85); CHECK(s.changes.size() == 1); }     // 5 s sustained
  { AdaptationController c; for (int i = 1; i <= 100; ++i) { PressureSample p; p.now = i * kNsPerSec; p.cpuAvailable = true; p.processCpuPct = 10; p.systemCpuPct = 90; c.update(p); } CHECK(c.level() > 0); }  // system CPU counts
  { Sim s; s.run(20, 20, 0.05); CHECK(s.changes.empty()); }               // exactly 5% misses is not ">5%"
  { Sim s; s.run(20, 20, 0.10); CHECK(!s.changes.empty() && near(s.changes[0].first, 5.0)); }
  { Sim s; s.run(10, 20, 0, 0.74); CHECK(s.changes.empty()); }
  { Sim s; s.run(10, 20, 0, 0.75); CHECK(!s.changes.empty() && near(s.changes[0].first, 2.0)); CHECK(s.changes.size() >= 2 && near(s.changes[1].first, 7.0)); }   // 2 s, then >=5 s apart
}
TEST(adapt_interrupted_pressure_does_not_accumulate) {
  Sim s; for (int i = 0; i < 20; ++i) { s.run(4, 95); s.run(1, 50); }       // 4 s on / 1 s off, never 5 s sustained
  CHECK(s.changes.empty());
}
TEST(adapt_missing_cpu_counter_never_triggers_cpu_pressure) {
  Sim s; s.run(30, 99, 0, 0, -1, false); CHECK(s.changes.empty());
}
TEST(adapt_hysteresis_and_restore_one_per_30s) {
  Sim s; s.run(20, 95); CHECK(s.c.level() == 4);
  s.changes.clear(); double t0 = nsToSec(s.t);
  s.run(60, 75);                                                          // 65..85 band: neither shed nor restore
  CHECK(s.changes.empty() && s.c.level() == 4);
  s.run(29, 50); CHECK(s.c.level() == 4);                                  // 29 s calm: not yet
  s.run(2, 50); CHECK(s.c.level() == 3);                                   // 30 s calm -> exactly one restore
  s.run(25, 50); CHECK(s.c.level() == 3);
  s.run(120, 50); CHECK(s.c.level() == 0);
  // restores are >= 30 s apart, in reverse order
  for (size_t i = 1; i < s.changes.size(); ++i) CHECK(s.changes[i].first - s.changes[i - 1].first >= 29.9);
  CHECK(s.changes.size() == 4 && s.changes[0].second == 3 && s.changes[3].second == 0);
  int up = 0; for (auto& e : s.c.events()) if (e.kind == AdaptEventKind::StepUp) ++up; CHECK(up == 4);
  (void)t0;
}
TEST(adapt_flapping_load_causes_no_oscillation) {
  Sim s; s.run(10, 95); CHECK(s.c.level() == 2);
  s.changes.clear();
  for (int i = 0; i < 100; ++i) { s.run(2, 90); s.run(2, 40); }          // 2 s hot / 2 s cool: neither 5 s sustained pressure nor 30 s calm
  CHECK(s.changes.empty() && s.c.level() == 2);
}
TEST(adapt_restore_requires_misses_and_queues_calm_too) {
  Sim s; s.run(10, 95); CHECK(s.c.level() == 2);
  s.run(100, 30, 0.02);  CHECK(s.c.level() == 2);                          // misses 2% >= 1%
  s.run(100, 30, 0, 0.3, 0.3); CHECK(s.c.level() == 2);                    // queue 30% >= 25%
  s.run(100, 30, 0, 0.1, 0.1); CHECK(s.c.level() == 0 || s.c.level() == 1);   // all calm: restoring
  s.run(100, 30, 0, 0.1, 0.1); CHECK(s.c.level() == 0);
}
TEST(adapt_pressure_during_calm_resets_restore_timer) {
  Sim s; s.run(10, 95); int lvl = s.c.level();
  s.run(25, 30); s.run(6, 95);                                             // calm 25 s then 6 s pressure
  CHECK(s.c.level() >= lvl);                                               // no restore happened; pressure may step further
  int l2 = s.c.level(); s.run(29, 30); CHECK(s.c.level() == l2);
}
TEST(adapt_artificial_load_feedback_loop_settles) {
  // load model: each level removes 4 points of CPU; starts at 98%. Sheds only until below trigger.
  AdaptationController c; Ns t = 0; std::vector<double> times;
  for (int i = 0; i < 1200; ++i) {
    PressureSample p; p.now = t; p.cpuAvailable = true; p.processCpuPct = 98.0 - 4.0 * c.level(); p.systemCpuPct = p.processCpuPct;
    int b = c.level(); c.update(p); if (c.level() != b) times.push_back(nsToSec(t));
    t += 100 * kNsPerMs;
  }
  CHECK(c.level() == 4);                                                   // 98-16 = 82 < 85
  CHECK(times.size() == 4 && near(times[0], 5) && near(times[3], 20));
}
TEST(adapt_locked_quality_warns_rejects_never_changes) {
  Sim s; s.c.setLocked(true); std::string reason; CHECK(s.c.tryAddWork("second output", &reason));
  s.run(60, 99);
  CHECK(s.c.level() == 0 && s.changes.empty() && s.c.state().previewFps == 30 && s.c.state().sceneFps == 30);
  CHECK(s.c.overloadActive()); CHECK(s.c.warnings() >= 10);                // roughly one per 5 s
  int w = 0; for (auto& e : s.c.events()) if (e.kind == AdaptEventKind::LockedOverloadWarning) ++w; CHECK(w >= 10);
  CHECK(!s.c.tryAddWork("second output", &reason)); CHECK(reason.find("locked") != std::string::npos); CHECK(s.c.rejectedWork() == 1);
  s.run(200, 30); CHECK(s.c.level() == 0 && !s.c.overloadActive());       // never changed, warning cleared once calm
  CHECK(s.c.tryAddWork("second output"));
}
TEST(adapt_lock_freezes_current_level_both_ways) {
  Sim s; s.run(15, 95); int lvl = s.c.level(); CHECK(lvl == 3);
  s.c.setLocked(true); s.run(300, 20); CHECK(s.c.level() == lvl);          // no restore while locked
  s.run(60, 99); CHECK(s.c.level() == lvl);                                // no further shedding while locked
  s.c.setLocked(false); s.run(31, 20); CHECK(s.c.level() == lvl - 1);
}
TEST(adapt_unsupported_steps_are_skipped) {
  AdaptationConfig cfg; cfg.trailsParticlesPresent = false; cfg.sourceRequestSupported = false; cfg.baseSceneFps = 25;
  AdaptationController c(cfg); std::vector<AdaptStep> want = {AdaptStep::PreviewFps5, AdaptStep::RenderScale, AdaptStep::SceneFps20, AdaptStep::SceneFps15};
  // base 25: SceneFps25 would be a no-op; it is skipped
  CHECK(c.ladder() == want); CHECK(c.maxLevel() == 4);
  AdaptationConfig c2; c2.baseSceneFps = 15; AdaptationController d(c2); for (auto st : d.ladder()) CHECK(st != AdaptStep::SceneFps25 && st != AdaptStep::SceneFps20 && st != AdaptStep::SceneFps15);
}
TEST(adapt_config_validation_ranges) {
  AdaptationConfig c; CHECK(c.validate().ok());
  c.cpuTriggerPct = 120; CHECK(!c.validate().ok()); c = AdaptationConfig(); c.restoreCpuPct = 84; CHECK(!c.validate().ok());     // no hysteresis gap
  c = AdaptationConfig(); c.restoreMiss = 0.06; CHECK(!c.validate().ok()); c = AdaptationConfig(); c.stepIntervalNs = 0; CHECK(!c.validate().ok());
  c = AdaptationConfig(); c.restoreQueue = 0.8; CHECK(!c.validate().ok());
  AdaptationConfig w; w.cpuTriggerPct = 500; w.restoreCpuPct = 99; w.stepIntervalNs = -5; AdaptationController ctl(w);
  CHECK(ctl.config().validate().ok());                                      // controller clamps into validated ranges
}
TEST(adapt_callbacks_and_time_monotonic) {
  AdaptationController c; int ev = 0, qc = 0; c.setEventCallback([&](const AdaptationEvent&) { ++ev; }); c.setQualityCallback([&](const QualityState&) { ++qc; });
  Sim s; s.c.setEventCallback([&](const AdaptationEvent&) { ++ev; }); s.c.setQualityCallback([&](const QualityState&) { ++qc; });
  s.run(12, 95); CHECK(ev == 2 && qc == 2);
  PressureSample old; old.now = 1; old.cpuAvailable = true; old.processCpuPct = 99; int l = s.c.level(); s.c.update(old); CHECK(s.c.level() == l);   // stale sample ignored
  for (int i = 0; i < 500; ++i) { s.c.update(PressureSample()); }              // never crashes / events bounded
  CHECK(s.c.events().size() <= 64);
  CHECK(std::string(adaptStepName(AdaptStep::RenderScale)) == "render_scale" && std::string(adaptEventKindName(AdaptEventKind::StepUp)) == "step_up");
}
TEST(adapt_added_work_policy_unlocked) {
  Sim s; std::string r; CHECK(s.c.tryAddWork("x")); s.run(10, 95); CHECK(!s.c.tryAddWork("video clip", &r)); CHECK(r.find("pressure") != std::string::npos);
  s.run(40, 30); CHECK(s.c.tryAddWork("video clip"));
}
int main() { return ss_test::runAll(); }
