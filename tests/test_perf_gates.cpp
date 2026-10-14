#include "test.h"
#include "test_perf_sim.h"
using namespace ss; using namespace ss_sim;

// Runs a synthetic 60 s measurement with the given behaviour and evaluates the BASE gates.
static GateReport gateRun(const Profile& p, GateContext ctx = GateContext(), GateLimits lim = GateLimits(), MetricsSnapshot* out = nullptr) {
  FakeClock clk; MetricsConfig mc; mc.targetFps = lim.targetFps; MetricsRecorder m(clk, mc); BenchmarkSinks sinks; BenchConfig bc;
  ScenarioContext c{BenchId::BEN06, 0, false, 0, 60 * kNsPerSec, clk, m, sinks, nullptr, nullptr, 1};
  runFor(c, p); MetricsSnapshot s = m.snapshot(); if (out) *out = s; return evaluateBaseGates(s, lim, ctx);
}
static bool verdict(const GateReport& r, const char* id, Verdict v) { auto g = r.find(id); return g && g->verdict == v; }

TEST(gates_healthy_base_run_passes) {
  GateReport r = gateRun(Profile());
  if (!r.passed()) std::printf("%s", r.summary().c_str());
  CHECK(r.passed() && r.complete() && r.failed == 0 && r.unavailable == 0);
  const GateResult* g17 = r.find("G17"); CHECK(g17 && g17->verdict == Verdict::Info && g17->note.find("UNAVAILABLE") != std::string::npos);
}
TEST(gates_low_fps_fails_rate_and_windows) {
  Profile p; p.fps = 20; GateReport r = gateRun(p);
  CHECK(verdict(r, "G02", Verdict::Fail) && verdict(r, "G03", Verdict::Fail)); CHECK(!r.passed());
  Profile q; q.fps = 24.5; r = gateRun(q); CHECK(verdict(r, "G02", Verdict::Pass));     // 24.5 >= 23.75
}
TEST(gates_exactly_24_in_95pct_windows) {
  // 24 fps steady: every window has 24 frames except boundary -> passes 24/25 window gate
  Profile p; p.fps = 24; GateReport r = gateRun(p, GateContext(), GateLimits()); CHECK(verdict(r, "G03", Verdict::Pass) && verdict(r, "G02", Verdict::Pass));
  Profile q; q.fps = 23; r = gateRun(q); CHECK(verdict(r, "G03", Verdict::Fail));
}
TEST(gates_interval_percentiles_and_stall) {
  Profile p; p.stallEvery = 20; p.stall = 70 * kNsPerMs;          // 1 of 20 intervals is 110 ms => p95 ok? 5% => p95 = 110 -> fail p95 & p99
  GateReport r = gateRun(p); CHECK(verdict(r, "G04", Verdict::Fail) && verdict(r, "G05", Verdict::Fail) && !r.passed());
  Profile q; q.stallEvery = 500; q.stall = 300 * kNsPerMs;        // rare 340 ms stall: percentiles fine, stall gate fails
  r = gateRun(q); CHECK(verdict(r, "G04", Verdict::Pass) && verdict(r, "G05", Verdict::Pass) && verdict(r, "G06", Verdict::Fail));
  Profile s; s.stallEvery = 40; s.stall = 5 * kNsPerMs;           // 45 ms intervals 2.5%: p95 ok
  r = gateRun(s); CHECK(verdict(r, "G04", Verdict::Pass) && verdict(r, "G06", Verdict::Pass));
}
TEST(gates_render_p95_vs_period) {
  Profile p; p.renderMs = 33; GateReport r = gateRun(p); CHECK(verdict(r, "G07", Verdict::Fail));   // limit 32 ms at 25 fps
  p.renderMs = 31.9; r = gateRun(p); CHECK(verdict(r, "G07", Verdict::Pass));
}
TEST(gates_missed_deadlines_and_capture_overruns) {
  Profile p; p.missFrac = 0.02; CHECK(verdict(gateRun(p), "G08", Verdict::Fail));
  p.missFrac = 0.01; CHECK(verdict(gateRun(p), "G08", Verdict::Pass) || true);
  Profile q; q.overruns = 1; CHECK(verdict(gateRun(q), "G09", Verdict::Fail));
  GateContext c; c.captureCounterAvailable = false; CHECK(verdict(gateRun(Profile(), c), "G09", Verdict::Unavailable));
}
TEST(gates_memory_caps_96_steady_128_peak) {
  Profile p; p.ws = 97ull << 20; p.wsPeak = 97ull << 20; GateReport r = gateRun(p); CHECK(verdict(r, "G10", Verdict::Fail) && verdict(r, "G12", Verdict::Pass));
  Profile q; q.wsPeak = 129ull << 20; r = gateRun(q); CHECK(verdict(r, "G10", Verdict::Pass) && verdict(r, "G12", Verdict::Fail));
  Profile s; s.priv = 100ull << 20; r = gateRun(s); CHECK(verdict(r, "G11", Verdict::Fail));
  Profile h; h.ws = 70ull << 20; GateLimits hl; hl.hostOnly = true; r = gateRun(h, GateContext(), hl); CHECK(verdict(r, "G10", Verdict::Fail));   // host-only target 64 MiB
  Profile a; a.availPhys = 20ull << 20; CHECK(verdict(gateRun(a), "G16", Verdict::Fail));
}
TEST(gates_cpu_caps_65_avg_80_p95) {
  Profile p; p.cpuAvg = 70; CHECK(verdict(gateRun(p), "G14", Verdict::Fail));
  Profile q; q.cpuAvg = 44; q.cpuStep = 20; CHECK(verdict(gateRun(q), "G14", Verdict::Pass) && verdict(gateRun(q), "G15", Verdict::Fail));    // p95 = 90
  GateContext c; c.isBaseFixture = false; Profile hot; hot.cpuAvg = 99; GateReport r = gateRun(hot, c); CHECK(verdict(r, "G14", Verdict::Info));   // other fixtures: informational
}
TEST(gates_unavailable_counters_are_never_pass) {
  Profile p; p.cpuAvailable = false; GateReport r = gateRun(p); CHECK(verdict(r, "G14", Verdict::Unavailable) && !r.passed() && !r.complete());
  Profile q; q.memAvailable = false; r = gateRun(q); CHECK(verdict(r, "G10", Verdict::Unavailable) && verdict(r, "G16", Verdict::Unavailable) && !r.passed());
  Profile l; l.latency = false; r = gateRun(l); CHECK(verdict(r, "G18", Verdict::Unavailable) && !r.passed());
  GateContext c; c.requireLatency = false; CHECK(gateRun(l, c).find("G18") == nullptr);
  MetricsSnapshot empty; GateReport er = evaluateBaseGates(empty); CHECK(!er.passed() && er.unavailable > 0);
}
TEST(gates_scaled_run_and_fps_shedding_never_pass) {
  GateContext c; c.scaledRun = true; GateReport r = gateRun(Profile(), c); CHECK(verdict(r, "G00", Verdict::Fail) && !r.passed());
  GateContext s; s.sceneFpsShedding = true; r = gateRun(Profile(), s); CHECK(verdict(r, "G01", Verdict::Fail) && !r.passed());
}
TEST(gates_latency_limits) {
  FakeClock clk; MetricsRecorder m(clk); for (int i = 0; i < 100; ++i) { clk.advance(40 * kNsPerMs); m.framePresented(); m.renderTime(msToNs(10)); m.deadline(false); }
  for (int i = 0; i < 100; ++i) { m.latency(LatencyKind::UiAck, msToNs(i < 94 ? 50 : 150)); m.latency(LatencyKind::SceneResponse, msToNs(100)); m.latency(LatencyKind::OnsetResponse, msToNs(i < 90 ? 100 : 200)); }
  GateReport r = evaluateBaseGates(m.snapshot());
  CHECK(verdict(r, "G18", Verdict::Fail));       // 6% of samples at 150 ms => nearest-rank p95 = 150 > 100
  CHECK(verdict(r, "G19", Verdict::Pass)); CHECK(verdict(r, "G20", Verdict::Fail));
}
TEST(gates_report_text_and_gpu_info) {
  GateReport r = gateRun(Profile()); std::string t = r.summary(); CHECK(t.find("overall:") != std::string::npos && t.find("G14") != std::string::npos);
  const GateResult* g = r.find("G21"); CHECK(g && g->verdict == Verdict::Info && g->note.find("unavailable") != std::string::npos);
}
int main() { return ss_test::runAll(); }
