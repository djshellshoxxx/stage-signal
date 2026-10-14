// Shared deterministic simulation helpers for test_perf_*.cpp (header only; not a test target).
#pragma once
#include "perf/metrics.h"
#include "perf/gates.h"
#include "perf/benchmark.h"

namespace ss_sim {
using namespace ss;
struct Profile {                         // synthetic "machine" behaviour for one run
  double fps = 25; double renderMs = 20; double jitterMs = 0; double cpuAvg = 50; double cpuStep = 0;
  uint64_t ws = 70ull << 20, priv = 72ull << 20, wsPeak = 80ull << 20; uint64_t availPhys = 100ull << 20;
  double missFrac = 0; int stallEvery = 0; Ns stall = 0; bool cpuAvailable = true, memAvailable = true, latency = true;
  uint64_t overruns = 0;
};
// Runs `durationNs` of fake time: frames at p.fps, counters sampled once per second.
inline void runFor(ScenarioContext& c, const Profile& p, Ns durationNs = 0) {
  FakeClock* fc = dynamic_cast<FakeClock*>(&c.clock); if (!fc) return;
  Ns end = durationNs ? c.startNs + durationNs : c.deadlineNs; Ns period = (Ns)(1e9 / p.fps); Ns lastSample = c.clock.nowNs(); int n = 0;
  FakeCpuCounters cpu; FakeMemCounters mem;
  while (fc->nowNs() < end) {
    fc->advance(period);
    if (p.stallEvery && (n % p.stallEvery) == p.stallEvery - 1) fc->advance(p.stall);
    c.metrics.renderTime(msToNs(p.renderMs + (n % 2 ? p.jitterMs : 0))); c.metrics.framePresented();
    bool miss = p.missFrac > 0 && (double)(n % 100) < p.missFrac * 100.0; c.metrics.deadline(miss);
    if (c.clock.nowNs() - lastSample >= kNsPerSec) {
      lastSample = c.clock.nowNs();
      cpu.cur.available = p.cpuAvailable; cpu.cur.logicalCpus = 1; cpu.cur.processPct = p.cpuAvg + p.cpuStep * (n % 3); cpu.cur.systemPct = p.cpuAvg + 5;
      cpu.cur.perCoreAvailable = true; cpu.cur.busiestCorePct = p.cpuAvg + 5;
      if (p.memAvailable) { mem.cur.workingSetBytes = Maybe<uint64_t>(p.ws); mem.cur.privateBytes = Maybe<uint64_t>(p.priv); mem.cur.availPhysBytes = Maybe<uint64_t>(p.availPhys); }
      c.metrics.sampleCounters(&cpu, &mem);
      if (p.latency) { c.metrics.latency(LatencyKind::UiAck, msToNs(40)); c.metrics.latency(LatencyKind::SceneResponse, msToNs(80)); c.metrics.latency(LatencyKind::OnsetResponse, msToNs(90)); }
    }
    ++n;
  }
  if (p.overruns) c.metrics.captureOverrun(p.overruns);
  if (p.memAvailable && p.wsPeak > p.ws) { mem.cur.workingSetBytes = Maybe<uint64_t>(p.wsPeak); c.metrics.sampleCounters(nullptr, &mem); }
}
}
