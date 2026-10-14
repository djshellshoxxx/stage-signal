// MOD-14 metrics (spec §7, REQ-P01): frame/present timing percentiles, missed deadlines, per-category drops, queue occupancy,
// CPU (process/system normalized across logical processors + busiest core) and memory via injectable counters.
// Anything a counter cannot supply is reported as UNAVAILABLE (never zero). Bounded memory; not for audio callbacks.
#pragma once
#include "core/types.h"
#include <string>
#include <vector>
#include <mutex>
#include <deque>

namespace ss {

template <class T> struct Maybe { bool available = false; T value{}; Maybe() = default; Maybe(T v) : available(true), value(v) {} };

struct Percentiles { bool available = false; size_t n = 0; double p50 = 0, p95 = 0, p99 = 0, max = 0; };
Percentiles computePercentiles(std::vector<double> v);      // nearest-rank; empty => unavailable

// ---- Counter ports (Windows adapter implements; fakes below) ------------------------------------
struct CpuSample {
  bool available = false;
  unsigned logicalCpus = 0;
  double processPct = 0;        // 0..100 NORMALIZED over all logical processors, usage since previous sample()
  double systemPct = 0;         // 0..100 normalized, whole machine
  bool perCoreAvailable = false;
  double busiestCorePct = 0;    // 0..100 of the single busiest logical processor
};
struct ICpuCounters { virtual ~ICpuCounters() = default; virtual CpuSample sample() = 0; };
struct MemSample {
  Maybe<uint64_t> workingSetBytes, privateBytes, availPhysBytes;
  Maybe<double> hardFaultsPerSec;      // app-caused paging indicator
};
struct IMemCounters { virtual ~IMemCounters() = default; virtual MemSample sample() = 0; };

struct FakeCpuCounters : ICpuCounters { CpuSample cur; CpuSample sample() override { return cur; } };
struct FakeMemCounters : IMemCounters { MemSample cur; MemSample sample() override { return cur; } };
// Counters that report nothing (used to prove "unavailable" is explicit).
struct NullCpuCounters : ICpuCounters { CpuSample sample() override { return CpuSample(); } };
struct NullMemCounters : IMemCounters { MemSample sample() override { return MemSample(); } };

enum class DropCat : uint8_t { Render, Present, Decode, Encode, Count_ };
const char* dropCatName(DropCat c);
enum class LatencyKind : uint8_t { UiAck, SceneResponse, OnsetResponse, Count_ };
const char* latencyKindName(LatencyKind k);

struct MetricsConfig {
  double targetFps = 25;           // configured presentation rate
  Ns windowNs = kNsPerSec;         // completed-frame window for REQ-P06 (1 s)
  Ns stallNs = 200 * kNsPerMs;     // render/present stall threshold
  size_t intervalRing = 8192, latencyRing = 2048, cpuRing = 4096, memRing = 4096, maxQueues = 16;
};

struct QueueStat { std::string name; size_t capacity = 0, current = 0, maxOccupancy = 0; double maxFraction = 0; };
struct CpuStats {
  bool available = false; size_t samples = 0, unavailableSamples = 0; unsigned logicalCpus = 0;
  double procAvg = 0, procP95 = 0, procMax = 0, sysAvg = 0, sysMax = 0;
  bool perCoreAvailable = false; double busiestCoreMax = 0;
};
struct MemStats {
  bool wsAvailable = false, privAvailable = false, availPhysAvailable = false, faultsAvailable = false;
  uint64_t wsSteady = 0 /*p95*/, wsPeak = 0, privSteady = 0 /*p95*/, privPeak = 0, availPhysMin = 0; double hardFaultsAvgPerSec = 0; size_t samples = 0;
};
struct MetricsSnapshot {
  double durationSec = 0; double targetFps = 25;
  uint64_t framesPresented = 0; double presentFps = 0;
  Percentiles presentIntervalMs, renderTimeMs; uint64_t presentStalls = 0, renderStalls = 0; double maxPresentIntervalMs = 0, maxRenderMs = 0;
  uint64_t deadlinesEvaluated = 0, deadlinesMissed = 0; double missedFraction = 0;
  uint64_t drops[4] = {0, 0, 0, 0}; uint64_t captureOverruns = 0;
  uint64_t windowsTotal = 0; uint32_t fpsHist[256] = {0};
  std::vector<QueueStat> queues; uint64_t queueNamesRejected = 0;
  CpuStats cpu; MemStats mem; Percentiles latencyMs[3];
  bool gpuAvailable = false; Percentiles gpuMs;       // GPU timing optional: explicit unavailable when no source
  uint64_t windowsBelow(double minFps) const;         // completed windows with fewer than ceil-free `minFps` frames (frames < minFps)
  uint64_t totalDrops() const { return drops[0] + drops[1] + drops[2] + drops[3]; }
};

class MetricsRecorder {
 public:
  explicit MetricsRecorder(IClock& clk, MetricsConfig cfg = MetricsConfig());
  void reset();                                       // starts a measured interval at clock.now()
  void framePresented();                              // presentation of a completed frame at clock.now()
  void framePresentedAt(Ns t);
  void renderTime(Ns dur);
  void deadline(bool missed);                         // one evaluation of a frame deadline
  void drop(DropCat c, uint64_t n = 1);
  void captureOverrun(uint64_t n = 1);
  void queueSample(const std::string& name, size_t size, size_t capacity);
  void latency(LatencyKind k, Ns d);
  void gpuFrameTime(Ns d);
  void sampleCounters(ICpuCounters* cpu, IMemCounters* mem);   // call about once per second
  MetricsSnapshot snapshot();                         // closes windows up to clock.now()
  const MetricsConfig& config() const { return cfg_; }
 private:
  void advance(Ns t);
  template <class T> static void ringPush(std::deque<T>& r, T v, size_t cap) { if (r.size() >= cap) r.pop_front(); r.push_back(v); }
  IClock& clk_; MetricsConfig cfg_; mutable std::mutex m_;
  Ns start_ = 0, winEnd_ = 0, lastPresent_ = 0; bool havePresent_ = false; uint32_t winCount_ = 0;
  uint64_t frames_ = 0, windows_ = 0; uint32_t hist_[256]; std::deque<double> intervals_, renders_, lat_[3], gpu_;
  uint64_t pStalls_ = 0, rStalls_ = 0; double maxInt_ = 0, maxRender_ = 0;
  uint64_t dlEval_ = 0, dlMiss_ = 0, drops_[4]; uint64_t overruns_ = 0;
  std::vector<QueueStat> queues_; uint64_t qRejected_ = 0;
  std::deque<double> cpuProc_; std::deque<uint64_t> ws_, priv_; CpuStats cpu_; double cpuProcSum_ = 0, cpuSysSum_ = 0;
  MemStats mem_; double faultSum_ = 0; size_t faultN_ = 0;
};

} // namespace ss
