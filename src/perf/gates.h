// MOD-14 BASE qualification gates (spec §7 "Proposed profile qualification" + REQ-P06). Evaluates a MetricsSnapshot into a
// pass/fail report. Honest by construction: a gate whose counter is unavailable is Unavailable (never Pass) and the report is
// not "passed"; scaled-duration runs and fps shedding can never pass. These are engineering release gates, not a claim.
#pragma once
#include "perf/metrics.h"
#include <string>
#include <vector>

namespace ss {

struct GateLimits {
  double targetFps = 25;                      // configured presentation rate
  double presentRateMinFraction = 0.95;       // >= 95% of configured rate overall
  double windowMinFps = 24; double windowFractionRequired = 0.95;   // >= 24 completed frames in 95% of 1 s windows
  double presentIntervalP95MaxMs = 50, presentIntervalP99MaxMs = 100, stallMaxMs = 200;
  double renderP95MaxFractionOfPeriod = 0.80; double missedMaxFraction = 0.01;
  uint64_t captureOverrunsMax = 0;
  uint64_t wsSteadyMax = 96ull << 20, privSteadyMax = 96ull << 20, wsPeakMax = 128ull << 20, privPeakMax = 128ull << 20;
  bool hostOnly = false; uint64_t hostOnlySteadyMax = 64ull << 20;          // host-only target <= 64 MiB
  double cpuAvgMaxPct = 65, cpuP95MaxPct = 80; uint64_t availPhysMin = 32ull << 20;
  double uiAckP95MaxMs = 100, sceneResponseP95MaxMs = 150, onsetResponseP95MaxMs = 150;
};
struct GateContext {
  bool scaledRun = false;                     // durations were scaled down (test/dry run): never qualifying
  bool sceneFpsShedding = false;              // adaptation lowered scene FPS target during the run
  bool isBaseFixture = true;                  // CPU % limits are defined for the single-core 800 MHz fixture
  bool captureCounterAvailable = true;        // overrun counter wired (BEN-06/soak); else Unavailable
  bool requireLatency = true;                 // REQ-P06 latency gates need measurements
};
enum class Verdict : uint8_t { Pass, Fail, Unavailable, Info };
const char* verdictName(Verdict v);
struct GateResult { std::string id, description, unit, note; Verdict verdict = Verdict::Unavailable; double measured = 0, limit = 0; };
struct GateReport {
  std::vector<GateResult> gates;
  size_t failed = 0, unavailable = 0, passedCount = 0;
  bool passed() const { return !gates.empty() && failed == 0 && unavailable == 0; }   // only true when everything was measured and met
  bool complete() const { return unavailable == 0; }
  std::string summary() const;                                                         // multi-line human text
  const GateResult* find(const std::string& id) const;
};
GateReport evaluateBaseGates(const MetricsSnapshot& m, const GateLimits& lim = GateLimits(), const GateContext& ctx = GateContext());

} // namespace ss
