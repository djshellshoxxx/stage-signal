#include "test.h"
#include "test_perf_sim.h"
#include "nlohmann/json.hpp"
using namespace ss; using namespace ss_sim;
using json = nlohmann::json;

static BenchEnvironment env() {
  BenchEnvironment e; e.fixtureId = "fixture-A"; e.sceneSeed = 42; e.appHash = "app-1"; e.os = "Windows 7 SP1 x86"; e.powerPlan = "High performance"; e.cpu = "Pentium III 800 MHz";
  e.gpu = "Intel i815"; e.gpuDriver = "6.14.10.4256"; e.monitorTopology = "1x 1024x768 + 1x 1280x1024"; e.compositionSizes = "640x360@25"; e.captureFormat = "48000/2/f32";
  e.encoder = "mf-h264"; e.bitrate = "2000k"; e.dependencyHashes["ffmpeg"] = "abc123"; e.mediaHashes["clip.mp4"] = "def456"; return e;
}
static BenchConfig onlyBase() { BenchConfig c; c.enabled = {BenchId::BEN01, BenchId::BEN06}; return c; }

TEST(bench_protocol_defaults_and_full_run_qualifies) {
  BenchConfig c; CHECK(c.warmupNs == 15 * kNsPerSec && c.runNs == 60 * kNsPerSec && c.runs == 3 && c.durationScale == 1.0);
  FakeClock clk; clk.wall = 1700000000000LL; FakeCpuCounters cpu; FakeMemCounters mem; BenchmarkRunner r(clk, onlyBase(), &cpu, &mem);
  Profile p; ScenarioFn fn = [&](ScenarioContext& ctx) { runFor(ctx, p); return ScenarioOutcome::Done(); };
  r.setScenario(BenchId::BEN01, fn); r.setScenario(BenchId::BEN06, fn);
  Ns t0 = clk.nowNs(); BenchResult R = r.run(env());
  CHECK(clk.nowNs() - t0 >= 2 * (15 + 3 * 60) * kNsPerSec);                // really ran 15 s warm-up + 3 x 60 s per scenario
  CHECK(!R.scaled && R.isolationOk && R.qualifying && R.scenarios.size() == 2);
  for (auto& s : R.scenarios) {
    CHECK(s.status == "completed" && s.runs.size() == 3 && s.gatesEvaluated);
    CHECK_NEAR(s.warmupSeconds, 15, 0.1); for (auto& m : s.runs) CHECK_NEAR(m.durationSec, 60, 0.1);
    if (!s.gatesPassed) for (auto& g : s.runGates) std::printf("%s", g.summary().c_str());
    CHECK(s.gatesPassed);
  }
  CHECK(R.ben06.present && R.ben06.admission == "admitted" && R.ben06.tier == "BASE");
  CHECK(R.ben06.cpuHeadroomAvailable && R.ben06.cpuHeadroomPct > 10 && R.ben06.frameHeadroomAvailable && R.ben06.frameHeadroomPct > 0);
  BenchProfile pr = R.toProfile(); CHECK(pr.qualifying && pr.gatesPassed && pr.passed(BenchId::BEN01) && pr.passed(BenchId::BEN06) && pr.tier == "BASE" && pr.wsPeakBytes >= (80ull << 20));
}
TEST(bench_scaled_runs_work_under_fake_clock_and_are_flagged) {
  BenchConfig c = onlyBase(); c.durationScale = 0.02; FakeClock clk; BenchmarkRunner r(clk, c);
  Profile p; ScenarioFn fn = [&](ScenarioContext& ctx) { runFor(ctx, p); return ScenarioOutcome::Done(); };
  r.setScenario(BenchId::BEN01, fn); r.setScenario(BenchId::BEN06, fn);
  BenchResult R = r.run(env()); CHECK(R.scaled && !R.qualifying && R.scenarios[0].status == "completed");
  CHECK(!R.scenarios[0].gatesPassed);                                       // G00 fails: shortened run can never qualify
  CHECK(R.ben06.tier == "untested (scaled run)" && R.ben06.admission == "untested");
  BenchProfile pr = R.toProfile(); CHECK(!pr.qualifying && !pr.passed(BenchId::BEN01));
}
TEST(bench_median_and_worst_of_three_runs) {
  BenchConfig c; c.enabled = {BenchId::BEN01}; c.durationScale = 0.05; FakeClock clk; BenchmarkRunner r(clk, c);
  double fps[3] = {25, 20, 24}; double wsPk[3] = {70, 90, 80};
  r.setScenario(BenchId::BEN01, [&](ScenarioContext& ctx) { Profile p; if (ctx.run >= 0) { p.fps = fps[ctx.run]; p.ws = (uint64_t)wsPk[ctx.run] << 20; p.wsPeak = p.ws; } runFor(ctx, p); return ScenarioOutcome::Done(); });
  BenchResult R = r.run(env()); auto& s = R.scenarios[0]; CHECK(s.runs.size() == 3);
  auto find = [&](const char* n) { for (auto& a : s.aggs) if (a.name == n) return a; return Agg(); };
  Agg f = find("present_fps"); CHECK(f.available); CHECK_NEAR(f.median, 24, 0.7); CHECK_NEAR(f.worst, 20, 0.7);
  Agg w = find("working_set_peak"); CHECK(w.available && w.worst == (double)(90ull << 20) && w.median == (double)(80ull << 20));
  CHECK(s.warmupFrames > 0);                                                // warm-up recorded separately, not in runs
  uint64_t runFrames = 0; for (auto& m : s.runs) runFrames += m.framesPresented; CHECK(runFrames > 0 && s.runs[0].framesPresented < 100);
}
TEST(bench_unavailable_counters_reported_explicitly) {
  BenchConfig c; c.enabled = {BenchId::BEN01}; c.durationScale = 0.05; FakeClock clk; BenchmarkRunner r(clk, c);
  r.setScenario(BenchId::BEN01, [&](ScenarioContext& ctx) { Profile p; p.cpuAvailable = false; p.memAvailable = false; runFor(ctx, p); return ScenarioOutcome::Done(); });
  BenchResult R = r.run(env()); json j = json::parse(R.toJson());
  auto& run0 = j["scenarios"][0]["runs"][0]; CHECK(run0["cpu"]["available"] == false); CHECK(run0["memory"]["working_set"] == "unavailable"); CHECK(run0["gpu_ms"] == "unavailable");
  bool found = false; for (auto& a : j["scenarios"][0]["summary"]) if (a["metric"] == "cpu_process_avg") { found = true; CHECK(a["available"] == false); } CHECK(found);
}
TEST(bench_skipped_and_missing_scenarios_are_recorded) {
  FakeClock clk; BenchConfig c; c.durationScale = 0.02; BenchmarkRunner r(clk, c); Profile p;
  r.setScenario(BenchId::BEN01, [&](ScenarioContext& ctx) { runFor(ctx, p); return ScenarioOutcome::Done(); });
  r.setScenario(BenchId::BEN03, [&](ScenarioContext&) { return ScenarioOutcome::Skip("no camera attached"); });
  r.setScenario(BenchId::BEN04, [&](ScenarioContext&) { return ScenarioOutcome::Fail("encoder init failed"); });
  BenchResult R = r.run(env()); CHECK(R.scenarios.size() == 6 && !R.qualifying);
  json j = json::parse(R.toJson()); bool cam = false, none = false; for (auto& s : j["skipped"]) { if (s["scenario"] == "BEN-03" && s["reason"] == "no camera attached") cam = true; if (s["scenario"] == "BEN-02") none = true; }
  CHECK(cam && none); CHECK(R.scenarios[3].status == "failed" && R.scenarios[3].reason == "encoder init failed");
  CHECK(R.ben06.present && R.ben06.admission == "untested");
}
TEST(bench_result_record_has_environment_and_is_json) {
  FakeClock clk; clk.wall = 1700000000000LL; BenchConfig c = onlyBase(); c.durationScale = 0.02; BenchmarkRunner r(clk, c); Profile p;
  ScenarioFn fn = [&](ScenarioContext& ctx) { runFor(ctx, p); return ScenarioOutcome::Done(); }; r.setScenario(BenchId::BEN01, fn); r.setScenario(BenchId::BEN06, fn);
  BenchEnvironment e = env(); e.extra["capture_note"] = "C:\\Users\\bob\\clip.mp4"; e.extra["stream_key"] = "hunter2"; e.encoder = "enc rtmps://u:pw@h/app/abcdefghij1234567890";
  BenchResult R = r.run(e); std::string js = R.toJson(); json j = json::parse(js);
  CHECK(j["environment"]["app_hash"] == "app-1" && j["environment"]["os"] == "Windows 7 SP1 x86" && j["environment"]["power_plan"] == "High performance");
  CHECK(j["environment"]["cpu"] == "Pentium III 800 MHz" && j["environment"]["gpu"] == "Intel i815" && j["environment"]["gpu_driver"] == "6.14.10.4256");
  CHECK(j["environment"]["dependency_hashes"]["ffmpeg"] == "abc123" && j["environment"]["media_hashes"]["clip.mp4"] == "def456" && j["environment"]["scene_seed"] == 42);
  CHECK(j["environment"]["monitor_topology"] == "1x 1024x768 + 1x 1280x1024" && j["environment"]["bitrate"] == "2000k");
  CHECK(js.find("bob") == std::string::npos && js.find("hunter2") == std::string::npos && js.find("pw@") == std::string::npos && js.find("abcdefghij1234") == std::string::npos);
  CHECK(j["protocol"]["scaled"] == true && j["scenarios"][0]["runs"][0].contains("present_interval_ms") && j["scenarios"][0]["runs"][0]["drops"].contains("encode"));
  CHECK(j["scenarios"][0]["runs"][0]["latency_ms"].contains("ui_ack")); CHECK(j["wall_ms"] == 1700000000000LL);
}
TEST(bench_comparable_keys) {
  auto key = [&](BenchEnvironment e) { FakeClock clk; BenchConfig c = onlyBase(); c.durationScale = 0.02; BenchmarkRunner r(clk, c); return r.run(e); };
  BenchResult a = key(env()), b = key(env()); CHECK(a.comparableKey == b.comparableKey && a.profileHash == b.profileHash && a.comparableKey.size() == 16);
  BenchEnvironment e2 = env(); e2.sceneSeed = 7; CHECK(key(e2).comparableKey != a.comparableKey);
  BenchEnvironment e3 = env(); e3.appHash = "app-2"; BenchResult c3 = key(e3); CHECK(c3.comparableKey == a.comparableKey && c3.appKey != a.appKey && c3.hardwareKey == a.hardwareKey);
  BenchEnvironment e4 = env(); e4.cpu = "Other"; BenchResult c4 = key(e4); CHECK(c4.hardwareKey != a.hardwareKey && c4.comparableKey == a.comparableKey);
  BenchEnvironment e5 = env(); e5.mediaHashes["clip.mp4"] = "changed"; CHECK(key(e5).comparableKey != a.comparableKey);
}
TEST(bench_isolation_violation_fails_scenario_and_result) {
  FakeClock clk; BenchConfig c; c.enabled = {BenchId::BEN04, BenchId::BEN05}; c.durationScale = 0.02; BenchmarkRunner r(clk, c); Profile p; int ben05Runs = 0;
  r.setScenario(BenchId::BEN04, [&](ScenarioContext& ctx) { uint8_t b[188] = {0}; ctx.sinks.recording.write(b, sizeof b); runFor(ctx, p); return ScenarioOutcome::Done(); });
  r.setScenario(BenchId::BEN05, [&](ScenarioContext& ctx) {
    ++ben05Runs; Status s = ctx.sinks.broadcast.begin("rtmps://u:secretpw@live.example.com/app/streamkey123456789?token=Z"); runFor(ctx, p);
    return s.ok() ? ScenarioOutcome::Done() : ScenarioOutcome::Done(); });
  BenchResult R = r.run(env()); CHECK(!R.isolationOk && !R.qualifying);
  CHECK(R.scenarios[0].status == "completed" && R.scenarios[0].sinkRecordBytes == 4 * 188 && R.scenarios[0].isolationViolations == 0);
  CHECK(R.scenarios[1].status == "failed" && R.scenarios[1].isolationViolations >= 1 && ben05Runs == 1);     // aborted after warm-up
  std::string js = R.toJson(); CHECK(js.find("secretpw") == std::string::npos && js.find("streamkey123") == std::string::npos); CHECK(js.find("isolation violation") != std::string::npos);
  CHECK(!R.toProfile().passed(BenchId::BEN05));
}
TEST(bench_ben05_uses_local_discard_receiver_only) {
  FakeClock clk; BenchConfig c; c.enabled = {BenchId::BEN05}; c.durationScale = 0.02; BenchmarkRunner r(clk, c); Profile p;
  r.setScenario(BenchId::BEN05, [&](ScenarioContext& ctx) { CHECK(ctx.sinks.broadcast.begin("srt://127.0.0.1:9000").ok()); uint8_t b[10] = {0}; ctx.sinks.broadcast.sendPacket(b, 10); runFor(ctx, p); ctx.sinks.broadcast.end(); return ScenarioOutcome::Done(); });
  BenchResult R = r.run(env()); CHECK(R.isolationOk && R.scenarios[0].status == "completed" && R.scenarios[0].sinkPackets >= 1);
}
int main() { return ss_test::runAll(); }
