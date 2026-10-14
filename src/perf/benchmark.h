// MOD-14 benchmark runner (spec §7 BEN-01..06, REQ-P01/P02). Protocol: 15 s warm-up (recorded separately), then 3 x 60 s runs,
// report median and worst. Scenario bodies are injected callbacks (the Windows layer plugs real renderers/encoders); the runner
// owns the clock discipline, metrics reset, isolation sinks and result record. Fully runnable under FakeClock with scaled
// durations (such results are flagged scaled/non-qualifying). Scenarios only ever see disposable sinks (REQ-P02).
#pragma once
#include "perf/metrics.h"
#include "perf/gates.h"
#include "perf/isolation.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ss {

enum class BenchId : uint8_t { BEN01, BEN02, BEN03, BEN04, BEN05, BEN06, Count_ };
const char* benchIdName(BenchId id);          // "BEN-01"
const char* benchIdDescription(BenchId id);   // workload text from spec §7

// Supplied by the host application: recorded verbatim (sanitized) in the result.
struct BenchEnvironment {
  std::string fixtureId = "unspecified"; uint64_t sceneSeed = 1;
  std::string appHash, os, powerPlan, cpu, gpu, gpuDriver, monitorTopology, compositionSizes, captureFormat, encoder, bitrate;
  std::map<std::string, std::string> dependencyHashes, mediaHashes, extra;
};
struct BenchConfig {
  Ns warmupNs = 15 * kNsPerSec; Ns runNs = 60 * kNsPerSec; int runs = 3;
  double durationScale = 1.0;                        // < 1 for tests/dry runs => result flagged scaled
  std::set<BenchId> enabled = {BenchId::BEN01, BenchId::BEN02, BenchId::BEN03, BenchId::BEN04, BenchId::BEN05, BenchId::BEN06};
  std::set<BenchId> gated = {BenchId::BEN01, BenchId::BEN06};   // scenarios evaluated against BASE gates
  GateLimits limits; bool isBaseFixture = true;
};

struct ScenarioContext {
  BenchId id = BenchId::BEN01; int run = -1; bool warmup = true;      // run index 0..N-1; warmup => metrics reported separately
  Ns startNs = 0, deadlineNs = 0;                                      // scenario loops while clock.nowNs() < deadlineNs
  IClock& clock; MetricsRecorder& metrics; BenchmarkSinks& sinks; ICpuCounters* cpu; IMemCounters* mem; uint64_t sceneSeed = 1;
  bool timeLeft() const { return clock.nowNs() < deadlineNs; }
  void sampleCounters() { metrics.sampleCounters(cpu, mem); }          // call about once per second
};
struct ScenarioOutcome {
  enum Kind : uint8_t { Ran, Skipped, Failed } kind = Ran; std::string reason;
  static ScenarioOutcome Done() { return {}; }
  static ScenarioOutcome Skip(std::string why) { ScenarioOutcome o; o.kind = Skipped; o.reason = std::move(why); return o; }
  static ScenarioOutcome Fail(std::string why) { ScenarioOutcome o; o.kind = Failed; o.reason = std::move(why); return o; }
};
using ScenarioFn = std::function<ScenarioOutcome(ScenarioContext&)>;

struct Agg { std::string name, unit; bool available = false; double median = 0, worst = 0; };
struct ScenarioResult {
  BenchId id = BenchId::BEN01; std::string status = "skipped"; std::string reason;   // completed | skipped | failed
  double warmupSeconds = 0, warmupFps = 0; uint64_t warmupFrames = 0;
  std::vector<MetricsSnapshot> runs; std::vector<Agg> aggs;
  bool gatesEvaluated = false; std::vector<GateReport> runGates; bool gatesPassed = false;
  uint64_t sinkDatagrams = 0, sinkRecordBytes = 0, sinkPackets = 0, isolationViolations = 0; std::vector<std::string> isolationEvents;
};
struct Ben06Summary {
  bool present = false; bool cpuHeadroomAvailable = false; double cpuHeadroomPct = 0;    // gate cpu-avg limit minus worst-run average
  bool frameHeadroomAvailable = false; double frameHeadroomPct = 0;                      // (80% of period - worst render p95) / period
  std::string admission;      // "admitted" | "not_admitted" | "untested"
  std::string tier;           // "BASE" | "below BASE" | "unverified" | "untested (scaled run)"
};
struct BenchProfile;
struct BenchResult {
  std::string schema = "stagesignal.bench/1"; BenchEnvironment env; int64_t wallMs = 0; Ns monoStartNs = 0;
  Ns warmupNs = 0, runNs = 0; int runsPerScenario = 0; double durationScale = 1; bool scaled = false;
  std::vector<ScenarioResult> scenarios; Ben06Summary ben06;
  bool isolationOk = true; bool qualifying = false;            // qualifying: unscaled, isolated, all requested scenarios ran
  std::string comparableKey, hardwareKey, appKey, profileHash; // FNV-1a/64 hex; equal comparableKey => same fixture, comparable records
  std::string toJson(int indent = 2) const;
  BenchProfile toProfile() const;
};

// Compact record kept in history and consumed by admission (IF-09 "profile hash").
struct BenchProfile {
  std::string profileHash, comparableKey, hardwareKey, appKey, tier = "untested"; int64_t wallMs = 0;
  bool qualifying = false, gatesPassed = false; bool cpuHeadroomAvailable = false; double cpuHeadroomPct = 0; bool frameHeadroomAvailable = false; double frameHeadroomPct = 0;
  uint64_t wsPeakBytes = 0; std::vector<std::string> scenariosPassed;     // e.g. "BEN-01"
  bool passed(BenchId id) const;
  std::string toJson() const;
  static bool fromJson(const std::string& s, BenchProfile& out);
};
class BenchHistory {                                            // bounded (16) list of profiles; persistence stores toJson()
 public:
  void add(const BenchProfile& p);
  const BenchProfile* findMatching(const std::string& hardwareKey, const std::string& appKey, int64_t nowWallMs, int64_t maxAgeMs) const;
  size_t size() const { return v_.size(); }
  std::string toJson() const; bool fromJson(const std::string& s);
 private:
  std::vector<BenchProfile> v_;
};

class BenchmarkRunner {
 public:
  BenchmarkRunner(IClock& clk, BenchConfig cfg = BenchConfig(), ICpuCounters* cpu = nullptr, IMemCounters* mem = nullptr);
  void setScenario(BenchId id, ScenarioFn fn) { fns_[id] = std::move(fn); }
  BenchResult run(const BenchEnvironment& env);                 // all enabled scenarios, in BEN order
  BenchmarkSinks& sinks() { return *sinks_; }                   // sinks of the scenario currently/last run
  static std::string fnv1a64(const std::string& s);
 private:
  IClock& clk_; BenchConfig cfg_; ICpuCounters* cpu_; IMemCounters* mem_; std::map<BenchId, ScenarioFn> fns_; std::unique_ptr<BenchmarkSinks> sinks_;
};

} // namespace ss
