#include "test.h"
#include "perf/soak.h"
using namespace ss;

static const double H = 3600.0;
TEST(soak_flat_noisy_series_is_bounded) {
  SeriesTrend t; for (int i = 0; i < 14400; ++i) t.add((Ns)(i * kNsPerSec), 70e6 + (i % 7) * 50e3 - (i % 3) * 30e3);   // 4 h flat
  TrendResult r = t.analyze(); CHECK(r.enough && !r.unbounded && std::fabs(r.growthTotal) < 1e6);
}
TEST(soak_linear_leak_detected) {
  SeriesTrend t; for (int i = 0; i < 14400; ++i) t.add((Ns)(i * kNsPerSec), 70e6 + i * 2000.0);                         // +2 kB/s = 7.2 MB/h
  TrendResult r = t.analyze(); CHECK(r.enough && r.unbounded && r.slopePerHour > 6e6 && r.slopePerHour < 8e6 && r.r2 > 0.99);
  CHECK(r.lastThirdMedian > r.firstThirdMedian);
}
TEST(soak_warmup_ramp_then_plateau_is_not_a_leak) {
  SeriesTrend t; for (int i = 0; i < 14400; ++i) { double v = i < 600 ? 40e6 + i * 50e3 : 70e6; t.add((Ns)(i * kNsPerSec), v + (i % 5) * 10e3); }
  CHECK(!t.analyze().unbounded);
}
TEST(soak_small_growth_under_allowance_ok) {
  SeriesTrend t; for (int i = 0; i < 14400; ++i) t.add((Ns)(i * kNsPerSec), 70e6 + i * 50.0);                          // 0.7 MB over 4 h
  CHECK(!t.analyze().unbounded);
}
TEST(soak_too_short_to_judge) {
  SeriesTrend t; for (int i = 0; i < 100; ++i) t.add((Ns)(i * kNsPerSec), 1e6 * i); CHECK(!t.analyze().enough && !t.analyze().unbounded);
  SeriesTrend e; CHECK(!e.analyze().enough);
}
TEST(soak_series_memory_bounded) {
  SeriesTrend t(64); for (int i = 0; i < 1000000; ++i) t.add((Ns)i * kNsPerSec, 5e6); CHECK(t.size() <= 64); CHECK(t.maxValue() == 5e6);
  SeriesTrend u(64); for (int i = 0; i < 100000; ++i) u.add((Ns)i * kNsPerSec, 1e6 + i * 1000.0); CHECK(u.size() <= 64 && u.analyze().unbounded);     // still detects after downsampling
}
static SoakReport run(double ws0, double wsSlope, size_t qMax, size_t qCap, SoakInputs in, double hours = 4.0) {
  SoakMonitor m; int n = (int)(hours * H);
  for (int i = 0; i < n; ++i) { Ns t = (Ns)i * kNsPerSec; m.sampleMemory(t, ws0 + wsSlope * i, ws0 + 1e6 + wsSlope * i); m.sampleQueue("encode", t, (size_t)(i % (qMax + 1)), qCap); }
  in.durationHours = hours; return m.finish(in);
}
TEST(soak_report_pass) {
  SoakInputs in; SoakReport r = run(70e6, 0, 3, 3, in); CHECK(r.pass && r.complete && r.failures.empty());
}
TEST(soak_report_fail_modes) {
  SoakInputs in; SoakReport r = run(70e6, 5000, 3, 3, in); CHECK(!r.pass && r.workingSet.unbounded);
  r = run(70e6, 0, 5, 3, in); CHECK(!r.pass);                                      // queue exceeded capacity
  SoakInputs c = in; c.crashed = true; CHECK(!run(70e6, 0, 3, 3, c).pass);
  c = in; c.stuckBlackout = true; CHECK(!run(70e6, 0, 3, 3, c).pass);
  c = in; c.undeclaredRecordingLossEvents = 1; CHECK(!run(70e6, 0, 3, 3, c).pass);
  c = in; c.captureOverruns = 1; CHECK(!run(70e6, 0, 3, 3, c).pass);
  CHECK(!run(70e6, 0, 3, 3, in, 1.0).pass);                                          // shorter than 4 h
}
TEST(soak_unavailable_capture_counter_is_not_a_pass) {
  SoakInputs in; in.captureCounterAvailable = false; SoakReport r = run(70e6, 0, 3, 3, in); CHECK(!r.pass && !r.complete && !r.unavailable.empty());
  SoakMonitor m; SoakInputs ok; ok.durationHours = 4; SoakReport e = m.finish(ok); CHECK(!e.pass && !e.complete);       // no samples => not verified
}
TEST(soak_growing_queue_detected) {
  SoakMonitor m; for (int i = 0; i < 14400; ++i) { m.sampleMemory((Ns)i * kNsPerSec, 70e6, 71e6); m.sampleQueue("q", (Ns)i * kNsPerSec, (size_t)(i / 100), 1000); }
  SoakInputs in; in.durationHours = 4; SoakReport r = m.finish(in); CHECK(!r.pass); bool q = false; for (auto& f : r.failures) if (f.find("q") != std::string::npos) q = true; CHECK(q);
}
int main() { return ss_test::runAll(); }
