#include "test.h"
#include "test_perf_sim.h"
#include "nlohmann/json.hpp"
using namespace ss;

TEST(perf_percentiles_nearest_rank) {
  std::vector<double> v; for (int i = 1; i <= 100; ++i) v.push_back(i);
  Percentiles p = computePercentiles(v); CHECK(p.available && p.n == 100 && p.p50 == 50 && p.p95 == 95 && p.p99 == 99 && p.max == 100);
  CHECK(!computePercentiles({}).available);
  Percentiles one = computePercentiles({7}); CHECK(one.p50 == 7 && one.p99 == 7);
}
TEST(perf_frame_windows_and_intervals) {
  FakeClock clk; MetricsConfig mc; mc.targetFps = 25; MetricsRecorder m(clk, mc);
  for (int i = 0; i < 250; ++i) { clk.advance(40 * kNsPerMs); m.framePresented(); }       // 10 s at 25 fps
  MetricsSnapshot s = m.snapshot();
  CHECK(s.framesPresented == 250 && s.windowsTotal == 10); CHECK_NEAR(s.presentFps, 25, 0.01);
  CHECK_NEAR(s.presentIntervalMs.p50, 40, 0.001); CHECK_NEAR(s.presentIntervalMs.p99, 40, 0.001);
  CHECK(s.windowsBelow(24) == 0 && s.fpsHist[24] == 1 && s.fpsHist[25] == 9);
  // one stalled second: 0 frames in a window plus a 1.3 s interval => stall
  clk.advance(1300 * kNsPerMs); m.framePresented(); s = m.snapshot();
  CHECK(s.presentStalls == 1 && s.maxPresentIntervalMs > 1299); CHECK(s.windowsBelow(24) >= 1);
}
TEST(perf_drops_are_separate_categories) {
  FakeClock clk; MetricsRecorder m(clk);
  m.drop(DropCat::Render, 3); m.drop(DropCat::Present, 1); m.drop(DropCat::Decode, 7); m.drop(DropCat::Encode, 2); m.captureOverrun(4);
  auto s = m.snapshot(); CHECK(s.drops[0] == 3 && s.drops[1] == 1 && s.drops[2] == 7 && s.drops[3] == 2 && s.captureOverruns == 4 && s.totalDrops() == 13);
  CHECK(std::string(dropCatName(DropCat::Decode)) == "decode");
}
TEST(perf_deadlines_and_queues) {
  FakeClock clk; MetricsConfig mc; mc.maxQueues = 2; MetricsRecorder m(clk, mc);
  for (int i = 0; i < 200; ++i) m.deadline(i % 50 == 0);
  m.queueSample("encode", 1, 3); m.queueSample("encode", 3, 3); m.queueSample("encode", 0, 3); m.queueSample("audio", 10, 100); m.queueSample("third", 1, 1);
  auto s = m.snapshot(); CHECK(s.deadlinesEvaluated == 200 && s.deadlinesMissed == 4); CHECK_NEAR(s.missedFraction, 0.02, 1e-9);
  CHECK(s.queues.size() == 2 && s.queueNamesRejected == 1);
  CHECK(s.queues[0].name == "encode" && s.queues[0].maxOccupancy == 3 && s.queues[0].current == 0 && s.queues[0].maxFraction == 1.0);
}
TEST(perf_unavailable_counters_explicit) {
  FakeClock clk; MetricsRecorder m(clk); NullCpuCounters nc; NullMemCounters nm;
  m.sampleCounters(&nc, &nm); m.sampleCounters(nullptr, nullptr);
  auto s = m.snapshot(); CHECK(!s.cpu.available && s.cpu.unavailableSamples == 2); CHECK(!s.mem.wsAvailable && !s.mem.privAvailable && !s.mem.availPhysAvailable && !s.mem.faultsAvailable);
  CHECK(!s.gpuAvailable && !s.presentIntervalMs.available && !s.latencyMs[0].available);
  FakeCpuCounters fc; fc.cur.available = true; fc.cur.logicalCpus = 4; fc.cur.processPct = 40; fc.cur.systemPct = 55; fc.cur.perCoreAvailable = false;
  m.sampleCounters(&fc, nullptr); s = m.snapshot(); CHECK(s.cpu.available && !s.cpu.perCoreAvailable && s.cpu.procAvg == 40 && s.cpu.logicalCpus == 4);
}
TEST(perf_cpu_mem_stats) {
  FakeClock clk; MetricsRecorder m(clk); FakeCpuCounters c; FakeMemCounters mm; c.cur.available = true; c.cur.logicalCpus = 2; c.cur.perCoreAvailable = true;
  for (int i = 0; i < 100; ++i) {
    c.cur.processPct = i < 95 ? 40 : 90; c.cur.systemPct = 50; c.cur.busiestCorePct = 60 + (i % 10);
    mm.cur.workingSetBytes = Maybe<uint64_t>(50ull << 20); mm.cur.privateBytes = Maybe<uint64_t>(60ull << 20); mm.cur.availPhysBytes = Maybe<uint64_t>((200 - i) * 1048576ull);
    mm.cur.hardFaultsPerSec = Maybe<double>(2.0); if (i == 50) mm.cur.workingSetBytes = Maybe<uint64_t>(120ull << 20);
    m.sampleCounters(&c, &mm);
  }
  auto s = m.snapshot();
  CHECK_NEAR(s.cpu.procAvg, 0.95 * 40 + 0.05 * 90, 1e-9); CHECK(s.cpu.procP95 == 40); CHECK(s.cpu.procMax == 90); CHECK(s.cpu.busiestCoreMax == 69);
  CHECK(s.mem.wsPeak == (120ull << 20) && s.mem.wsSteady == (50ull << 20) && s.mem.privSteady == (60ull << 20));
  CHECK(s.mem.availPhysMin == 101ull * 1048576); CHECK_NEAR(s.mem.hardFaultsAvgPerSec, 2.0, 1e-9);
}
TEST(perf_recorder_memory_bounded) {
  FakeClock clk; MetricsConfig mc; mc.intervalRing = 100; mc.cpuRing = 50; MetricsRecorder m(clk, mc); FakeCpuCounters c; c.cur.available = true; c.cur.logicalCpus = 1;
  for (int i = 0; i < 100000; ++i) { clk.advance(10 * kNsPerMs); m.framePresented(); m.renderTime(kNsPerMs); if (i % 100 == 0) m.sampleCounters(&c, nullptr); }
  auto s = m.snapshot(); CHECK(s.presentIntervalMs.n == 100 && s.renderTimeMs.n == 100); CHECK(s.framesPresented == 100000 && s.windowsTotal == 1000);
}
TEST(perf_reset_starts_new_interval) {
  FakeClock clk; MetricsRecorder m(clk); for (int i = 0; i < 50; ++i) { clk.advance(40 * kNsPerMs); m.framePresented(); m.drop(DropCat::Render); }
  m.reset(); auto s = m.snapshot(); CHECK(s.framesPresented == 0 && s.totalDrops() == 0 && s.windowsTotal == 0);
}
int main() { return ss_test::runAll(); }
