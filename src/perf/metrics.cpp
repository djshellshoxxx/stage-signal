#include "perf/metrics.h"
#include <algorithm>
#include <cmath>

namespace ss {

Percentiles computePercentiles(std::vector<double> v) {
  Percentiles p; if (v.empty()) return p;
  std::sort(v.begin(), v.end()); p.available = true; p.n = v.size();
  auto at = [&](double q) { size_t r = (size_t)std::ceil(q * (double)v.size()); if (r < 1) r = 1; return v[std::min(r, v.size()) - 1]; };
  p.p50 = at(0.50); p.p95 = at(0.95); p.p99 = at(0.99); p.max = v.back(); return p;
}
static Percentiles pct(const std::deque<double>& d) { return computePercentiles(std::vector<double>(d.begin(), d.end())); }
const char* dropCatName(DropCat c) { static const char* n[] = {"render", "present", "decode", "encode"}; return (size_t)c < 4 ? n[(size_t)c] : "?"; }
const char* latencyKindName(LatencyKind k) { static const char* n[] = {"ui_ack", "scene_response", "onset_response"}; return (size_t)k < 3 ? n[(size_t)k] : "?"; }

uint64_t MetricsSnapshot::windowsBelow(double minFps) const {
  uint64_t n = 0; for (int i = 0; i < 256; ++i) if ((double)i < minFps) n += fpsHist[i]; return n;
}

MetricsRecorder::MetricsRecorder(IClock& clk, MetricsConfig cfg) : clk_(clk), cfg_(cfg) { if (cfg_.windowNs < kNsPerMs) cfg_.windowNs = kNsPerSec; reset(); }

void MetricsRecorder::reset() {
  std::lock_guard<std::mutex> g(m_);
  start_ = clk_.nowNs(); winEnd_ = start_ + cfg_.windowNs; winCount_ = 0; havePresent_ = false; lastPresent_ = 0; frames_ = 0; windows_ = 0;
  for (auto& h : hist_) h = 0;
  intervals_.clear(); renders_.clear(); for (auto& l : lat_) l.clear(); gpu_.clear();
  pStalls_ = rStalls_ = 0; maxInt_ = maxRender_ = 0; dlEval_ = dlMiss_ = 0; for (auto& d : drops_) d = 0; overruns_ = 0;
  queues_.clear(); qRejected_ = 0; cpuProc_.clear(); ws_.clear(); priv_.clear(); cpu_ = CpuStats(); cpuProcSum_ = cpuSysSum_ = 0; mem_ = MemStats(); faultSum_ = 0; faultN_ = 0;
}
void MetricsRecorder::advance(Ns t) {
  while (t >= winEnd_) { ++hist_[std::min<uint32_t>(winCount_, 255)]; ++windows_; winCount_ = 0; winEnd_ += cfg_.windowNs; }
}
void MetricsRecorder::framePresented() { framePresentedAt(clk_.nowNs()); }
void MetricsRecorder::framePresentedAt(Ns t) {
  std::lock_guard<std::mutex> g(m_);
  advance(t); ++winCount_; ++frames_;
  if (havePresent_) {
    double ms = nsToMs(t - lastPresent_); ringPush(intervals_, ms, cfg_.intervalRing);
    if (ms > maxInt_) maxInt_ = ms;
    if (t - lastPresent_ > cfg_.stallNs) ++pStalls_;
  }
  havePresent_ = true; lastPresent_ = t;
}
void MetricsRecorder::renderTime(Ns d) {
  std::lock_guard<std::mutex> g(m_); double ms = nsToMs(d); ringPush(renders_, ms, cfg_.intervalRing);
  if (ms > maxRender_) maxRender_ = ms;
  if (d > cfg_.stallNs) ++rStalls_;
}
void MetricsRecorder::deadline(bool missed) { std::lock_guard<std::mutex> g(m_); ++dlEval_; if (missed) ++dlMiss_; }
void MetricsRecorder::drop(DropCat c, uint64_t n) { std::lock_guard<std::mutex> g(m_); if ((size_t)c < 4) drops_[(size_t)c] += n; }
void MetricsRecorder::captureOverrun(uint64_t n) { std::lock_guard<std::mutex> g(m_); overruns_ += n; }
void MetricsRecorder::latency(LatencyKind k, Ns d) { std::lock_guard<std::mutex> g(m_); if ((size_t)k < 3) ringPush(lat_[(size_t)k], nsToMs(d), cfg_.latencyRing); }
void MetricsRecorder::gpuFrameTime(Ns d) { std::lock_guard<std::mutex> g(m_); ringPush(gpu_, nsToMs(d), cfg_.intervalRing); }
void MetricsRecorder::queueSample(const std::string& name, size_t size, size_t cap) {
  std::lock_guard<std::mutex> g(m_);
  for (auto& q : queues_) if (q.name == name) {
    q.current = size; q.capacity = cap; if (size > q.maxOccupancy) q.maxOccupancy = size;
    double f = cap ? (double)size / (double)cap : 0; if (f > q.maxFraction) q.maxFraction = f; return; }
  if (queues_.size() >= cfg_.maxQueues) { ++qRejected_; return; }
  QueueStat q; q.name = name.substr(0, 48); q.capacity = cap; q.current = q.maxOccupancy = size; q.maxFraction = cap ? (double)size / (double)cap : 0; queues_.push_back(q);
}
void MetricsRecorder::sampleCounters(ICpuCounters* cpu, IMemCounters* mem) {
  std::lock_guard<std::mutex> g(m_);
  if (cpu) {
    CpuSample s = cpu->sample();
    if (!s.available) ++cpu_.unavailableSamples;
    else {
      cpu_.available = true; ++cpu_.samples; cpu_.logicalCpus = s.logicalCpus; ringPush(cpuProc_, s.processPct, cfg_.cpuRing);
      cpuProcSum_ += s.processPct; cpuSysSum_ += s.systemPct; cpu_.procMax = std::max(cpu_.procMax, s.processPct); cpu_.sysMax = std::max(cpu_.sysMax, s.systemPct);
      if (s.perCoreAvailable) { cpu_.perCoreAvailable = true; cpu_.busiestCoreMax = std::max(cpu_.busiestCoreMax, s.busiestCorePct); }
    }
  } else ++cpu_.unavailableSamples;
  if (mem) {
    MemSample s = mem->sample(); ++mem_.samples;
    if (s.workingSetBytes.available) { mem_.wsAvailable = true; ringPush(ws_, s.workingSetBytes.value, cfg_.memRing); mem_.wsPeak = std::max(mem_.wsPeak, s.workingSetBytes.value); }
    if (s.privateBytes.available) { mem_.privAvailable = true; ringPush(priv_, s.privateBytes.value, cfg_.memRing); mem_.privPeak = std::max(mem_.privPeak, s.privateBytes.value); }
    if (s.availPhysBytes.available) { mem_.availPhysMin = mem_.availPhysAvailable ? std::min(mem_.availPhysMin, s.availPhysBytes.value) : s.availPhysBytes.value; mem_.availPhysAvailable = true; }
    if (s.hardFaultsPerSec.available) { mem_.faultsAvailable = true; faultSum_ += s.hardFaultsPerSec.value; ++faultN_; }
  }
}
static uint64_t p95u(const std::deque<uint64_t>& d) {
  std::vector<double> v(d.begin(), d.end()); return (uint64_t)computePercentiles(v).p95;
}
MetricsSnapshot MetricsRecorder::snapshot() {
  std::lock_guard<std::mutex> g(m_);
  Ns now = clk_.nowNs(); advance(now);
  MetricsSnapshot s; s.targetFps = cfg_.targetFps; s.durationSec = nsToSec(now - start_); s.framesPresented = frames_;
  s.presentFps = s.durationSec > 0 ? (double)frames_ / s.durationSec : 0;
  s.presentIntervalMs = pct(intervals_); s.renderTimeMs = pct(renders_); s.presentStalls = pStalls_; s.renderStalls = rStalls_;
  s.maxPresentIntervalMs = maxInt_; s.maxRenderMs = maxRender_;
  s.deadlinesEvaluated = dlEval_; s.deadlinesMissed = dlMiss_; s.missedFraction = dlEval_ ? (double)dlMiss_ / (double)dlEval_ : 0;
  for (int i = 0; i < 4; ++i) s.drops[i] = drops_[i];
  s.captureOverruns = overruns_; s.windowsTotal = windows_; for (int i = 0; i < 256; ++i) s.fpsHist[i] = hist_[i];
  s.queues = queues_; s.queueNamesRejected = qRejected_;
  s.cpu = cpu_; if (cpu_.samples) { s.cpu.procAvg = cpuProcSum_ / (double)cpu_.samples; s.cpu.sysAvg = cpuSysSum_ / (double)cpu_.samples; s.cpu.procP95 = pct(cpuProc_).p95; }
  s.mem = mem_; s.mem.wsSteady = ws_.empty() ? 0 : p95u(ws_); s.mem.privSteady = priv_.empty() ? 0 : p95u(priv_);
  s.mem.hardFaultsAvgPerSec = faultN_ ? faultSum_ / (double)faultN_ : 0;
  for (int i = 0; i < 3; ++i) s.latencyMs[i] = pct(lat_[i]);
  s.gpuAvailable = !gpu_.empty(); s.gpuMs = pct(gpu_);
  return s;
}

} // namespace ss
