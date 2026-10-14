#include "perf/gates.h"
#include <algorithm>
#include <cstdio>

namespace ss {

const char* verdictName(Verdict v) { static const char* n[] = {"pass", "fail", "unavailable", "info"}; return (size_t)v < 4 ? n[(size_t)v] : "?"; }
const GateResult* GateReport::find(const std::string& id) const { for (auto& g : gates) if (g.id == id) return &g; return nullptr; }
std::string GateReport::summary() const {
  std::string s; char b[256];
  for (auto& g : gates) {
    std::snprintf(b, sizeof b, "%-4s %-22s %-11s measured=%.3f limit=%.3f %s %s\n", g.id.c_str(), g.description.c_str(), verdictName(g.verdict), g.measured, g.limit, g.unit.c_str(), g.note.c_str());
    s += b;
  }
  std::snprintf(b, sizeof b, "overall: %s (%zu pass, %zu fail, %zu unavailable)\n", passed() ? "PASS" : (failed ? "FAIL" : "NOT VERIFIED"), passedCount, failed, unavailable); s += b;
  return s;
}

namespace {
struct Builder {
  GateReport r;
  void add(const char* id, const char* desc, Verdict v, double meas, double lim, const char* unit, const std::string& note = std::string()) {
    GateResult g; g.id = id; g.description = desc; g.verdict = v; g.measured = meas; g.limit = lim; g.unit = unit; g.note = note; r.gates.push_back(g);
    if (v == Verdict::Fail) ++r.failed; else if (v == Verdict::Unavailable) ++r.unavailable; else if (v == Verdict::Pass) ++r.passedCount;
  }
  void le(const char* id, const char* desc, bool avail, double meas, double lim, const char* unit, const std::string& note = std::string()) {
    if (!avail) add(id, desc, Verdict::Unavailable, 0, lim, unit, "counter unavailable"); else add(id, desc, meas <= lim ? Verdict::Pass : Verdict::Fail, meas, lim, unit, note);
  }
  void ge(const char* id, const char* desc, bool avail, double meas, double lim, const char* unit, const std::string& note = std::string()) {
    if (!avail) add(id, desc, Verdict::Unavailable, 0, lim, unit, "counter unavailable"); else add(id, desc, meas >= lim ? Verdict::Pass : Verdict::Fail, meas, lim, unit, note);
  }
};
const double MiB = 1048576.0;
}

GateReport evaluateBaseGates(const MetricsSnapshot& m, const GateLimits& L, const GateContext& C) {
  Builder b; const double period = 1000.0 / (L.targetFps > 0 ? L.targetFps : 25.0);
  b.add("G00", "full-duration protocol", C.scaledRun ? Verdict::Fail : Verdict::Pass, C.scaledRun ? 1 : 0, 0, "", C.scaledRun ? "scaled/shortened run is not qualifying evidence" : "");
  b.add("G01", "no fps shedding", C.sceneFpsShedding ? Verdict::Fail : Verdict::Pass, C.sceneFpsShedding ? 1 : 0, 0, "", C.sceneFpsShedding ? "adaptive shedding lowered scene FPS; it is recovery, not a way to pass" : "");
  b.ge("G02", "present rate", m.durationSec > 0, m.presentFps, L.presentRateMinFraction * L.targetFps, "fps");
  if (m.windowsTotal == 0) b.add("G03", "24fps windows", Verdict::Unavailable, 0, L.windowFractionRequired, "fraction", "no completed 1 s window");
  else { double ok = 1.0 - (double)m.windowsBelow(L.windowMinFps) / (double)m.windowsTotal; b.ge("G03", "24fps windows", true, ok, L.windowFractionRequired, "fraction", "windows with >= windowMinFps completed frames"); }
  b.le("G04", "present interval p95", m.presentIntervalMs.available, m.presentIntervalMs.p95, L.presentIntervalP95MaxMs, "ms");
  b.le("G05", "present interval p99", m.presentIntervalMs.available, m.presentIntervalMs.p99, L.presentIntervalP99MaxMs, "ms");
  { double st = std::max(m.maxPresentIntervalMs, m.maxRenderMs); b.le("G06", "no render stall", m.presentIntervalMs.available || m.renderTimeMs.available, st, L.stallMaxMs, "ms"); }
  b.le("G07", "render p95 vs period", m.renderTimeMs.available, m.renderTimeMs.p95, L.renderP95MaxFractionOfPeriod * period, "ms");
  b.le("G08", "missed deadlines", m.deadlinesEvaluated > 0, m.missedFraction, L.missedMaxFraction, "fraction");
  b.le("G09", "capture overruns", C.captureCounterAvailable, (double)m.captureOverruns, (double)L.captureOverrunsMax, "count");
  const uint64_t wsS = L.hostOnly ? L.hostOnlySteadyMax : L.wsSteadyMax, pvS = L.hostOnly ? L.hostOnlySteadyMax : L.privSteadyMax;
  b.le("G10", "working set steady", m.mem.wsAvailable, (double)m.mem.wsSteady / MiB, (double)wsS / MiB, "MiB", "p95 of samples");
  b.le("G11", "private bytes steady", m.mem.privAvailable, (double)m.mem.privSteady / MiB, (double)pvS / MiB, "MiB", "p95 of samples");
  b.le("G12", "working set peak", m.mem.wsAvailable, (double)m.mem.wsPeak / MiB, (double)L.wsPeakMax / MiB, "MiB");
  b.le("G13", "private bytes peak", m.mem.privAvailable, (double)m.mem.privPeak / MiB, (double)L.privPeakMax / MiB, "MiB");
  if (C.isBaseFixture) {
    b.le("G14", "cpu average", m.cpu.available, m.cpu.procAvg, L.cpuAvgMaxPct, "pct", "process, normalized");
    b.le("G15", "cpu p95 (1 s)", m.cpu.available, m.cpu.procP95, L.cpuP95MaxPct, "pct");
  } else {
    b.add("G14", "cpu average", Verdict::Info, m.cpu.procAvg, L.cpuAvgMaxPct, "pct", "not the BASE fixture: informational only");
    b.add("G15", "cpu p95 (1 s)", Verdict::Info, m.cpu.procP95, L.cpuP95MaxPct, "pct", "not the BASE fixture: informational only");
  }
  b.ge("G16", "available physical", m.mem.availPhysAvailable, (double)m.mem.availPhysMin / MiB, (double)L.availPhysMin / MiB, "MiB", "minimum observed");
  // Spec gives no numeric paging threshold ("avoid sustained app-caused paging"): always Info, but unavailability is stated explicitly.
  if (m.mem.faultsAvailable) b.add("G17", "hard faults (paging)", Verdict::Info, m.mem.hardFaultsAvgPerSec, 0, "per_s", "no numeric limit in spec: qualifier must assess sustained app-caused paging");
  else b.add("G17", "hard faults (paging)", Verdict::Info, 0, 0, "per_s", "counter UNAVAILABLE: paging not assessed");
  if (C.requireLatency) {
    b.le("G18", "ui ack p95", m.latencyMs[0].available, m.latencyMs[0].p95, L.uiAckP95MaxMs, "ms");
    b.le("G19", "scene response p95", m.latencyMs[1].available, m.latencyMs[1].p95, L.sceneResponseP95MaxMs, "ms");
    b.le("G20", "onset response p95", m.latencyMs[2].available, m.latencyMs[2].p95, L.onsetResponseP95MaxMs, "ms");
  }
  if (!m.gpuAvailable) b.add("G21", "gpu time", Verdict::Info, 0, 0, "ms", "GPU timing unavailable (optional)");
  else b.add("G21", "gpu time", Verdict::Info, m.gpuMs.p95, 0, "ms", "p95, optional");
  return b.r;
}

} // namespace ss
