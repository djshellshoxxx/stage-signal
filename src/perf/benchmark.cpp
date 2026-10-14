#include "perf/benchmark.h"
#include "core/security.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstdio>

namespace ss {
using json = nlohmann::json;

const char* benchIdName(BenchId id) { static const char* n[] = {"BEN-01", "BEN-02", "BEN-03", "BEN-04", "BEN-05", "BEN-06"}; return (size_t)id < 6 ? n[(size_t)id] : "?"; }
const char* benchIdDescription(BenchId id) {
  static const char* d[] = {"Basic scenes + audio bands + text", "Duplicate and independent outputs", "Camera treatments + GIF + one video clip",
    "Program encode to disposable local test file", "Broadcast encoder/muxer to local discard/test receiver, never remote", "Selected combined performance configuration"};
  return (size_t)id < 6 ? d[(size_t)id] : "?";
}

std::string BenchmarkRunner::fnv1a64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL; for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  char b[20]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return b;
}

namespace {
std::string S(const std::string& s) { return sec::sanitizeLibraryError(s, 512); }
json pj(const Percentiles& p) {
  json j; j["available"] = p.available; if (!p.available) return j;
  j["n"] = p.n; j["p50"] = p.p50; j["p95"] = p.p95; j["p99"] = p.p99; j["max"] = p.max; return j;
}
json mj(const MetricsSnapshot& m) {
  json j; j["duration_s"] = m.durationSec; j["target_fps"] = m.targetFps; j["frames_presented"] = m.framesPresented; j["present_fps"] = m.presentFps;
  j["present_interval_ms"] = pj(m.presentIntervalMs); j["render_time_ms"] = pj(m.renderTimeMs);
  j["present_stalls"] = m.presentStalls; j["render_stalls"] = m.renderStalls; j["max_present_interval_ms"] = m.maxPresentIntervalMs; j["max_render_ms"] = m.maxRenderMs;
  j["deadlines_evaluated"] = m.deadlinesEvaluated; j["deadlines_missed"] = m.deadlinesMissed; j["missed_fraction"] = m.missedFraction;
  json d; for (int i = 0; i < 4; ++i) d[dropCatName((DropCat)i)] = m.drops[i]; j["drops"] = d; j["capture_overruns"] = m.captureOverruns;
  j["windows_total"] = m.windowsTotal; j["windows_below_24fps"] = m.windowsBelow(24);
  json q = json::array(); for (auto& s : m.queues) q.push_back({{"name", S(s.name)}, {"capacity", s.capacity}, {"current", s.current}, {"max_occupancy", s.maxOccupancy}, {"max_fraction", s.maxFraction}});
  j["queues"] = q;
  json c; c["available"] = m.cpu.available; if (m.cpu.available) { c["logical_cpus"] = m.cpu.logicalCpus; c["process_avg_pct"] = m.cpu.procAvg; c["process_p95_pct"] = m.cpu.procP95; c["process_max_pct"] = m.cpu.procMax;
    c["system_avg_pct"] = m.cpu.sysAvg; c["system_max_pct"] = m.cpu.sysMax; c["busiest_core"] = m.cpu.perCoreAvailable ? json(m.cpu.busiestCoreMax) : json("unavailable"); }
  j["cpu"] = c;
  json me; me["working_set"] = m.mem.wsAvailable ? json({{"steady_p95", m.mem.wsSteady}, {"peak", m.mem.wsPeak}}) : json("unavailable");
  me["private_bytes"] = m.mem.privAvailable ? json({{"steady_p95", m.mem.privSteady}, {"peak", m.mem.privPeak}}) : json("unavailable");
  me["available_physical_min"] = m.mem.availPhysAvailable ? json(m.mem.availPhysMin) : json("unavailable");
  me["hard_faults_per_s"] = m.mem.faultsAvailable ? json(m.mem.hardFaultsAvgPerSec) : json("unavailable"); j["memory"] = me;
  json l; for (int i = 0; i < 3; ++i) l[latencyKindName((LatencyKind)i)] = pj(m.latencyMs[i]); j["latency_ms"] = l;
  j["gpu_ms"] = m.gpuAvailable ? pj(m.gpuMs) : json("unavailable");
  return j;
}
json gj(const GateReport& r) {
  json a = json::array();
  for (auto& g : r.gates) a.push_back({{"id", g.id}, {"gate", g.description}, {"verdict", verdictName(g.verdict)}, {"measured", g.measured}, {"limit", g.limit}, {"unit", g.unit}, {"note", g.note}});
  return json({{"passed", r.passed()}, {"complete", r.complete()}, {"failed", r.failed}, {"unavailable", r.unavailable}, {"gates", a}});
}
double median(std::vector<double> v) { std::sort(v.begin(), v.end()); size_t n = v.size(); if (!n) return 0; return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]); }

struct Extract { const char* name; const char* unit; bool higherIsBetter; std::function<bool(const MetricsSnapshot&, double&)> get; };
std::vector<Extract> extractors() {
  return {
    {"present_fps", "fps", true, [](const MetricsSnapshot& m, double& v) { v = m.presentFps; return m.durationSec > 0; }},
    {"present_interval_p95", "ms", false, [](const MetricsSnapshot& m, double& v) { v = m.presentIntervalMs.p95; return m.presentIntervalMs.available; }},
    {"present_interval_p99", "ms", false, [](const MetricsSnapshot& m, double& v) { v = m.presentIntervalMs.p99; return m.presentIntervalMs.available; }},
    {"max_stall", "ms", false, [](const MetricsSnapshot& m, double& v) { v = std::max(m.maxPresentIntervalMs, m.maxRenderMs); return m.presentIntervalMs.available || m.renderTimeMs.available; }},
    {"render_p50", "ms", false, [](const MetricsSnapshot& m, double& v) { v = m.renderTimeMs.p50; return m.renderTimeMs.available; }},
    {"render_p95", "ms", false, [](const MetricsSnapshot& m, double& v) { v = m.renderTimeMs.p95; return m.renderTimeMs.available; }},
    {"render_p99", "ms", false, [](const MetricsSnapshot& m, double& v) { v = m.renderTimeMs.p99; return m.renderTimeMs.available; }},
    {"missed_fraction", "fraction", false, [](const MetricsSnapshot& m, double& v) { v = m.missedFraction; return m.deadlinesEvaluated > 0; }},
    {"drops_total", "count", false, [](const MetricsSnapshot& m, double& v) { v = (double)m.totalDrops(); return true; }},
    {"capture_overruns", "count", false, [](const MetricsSnapshot& m, double& v) { v = (double)m.captureOverruns; return true; }},
    {"cpu_process_avg", "pct", false, [](const MetricsSnapshot& m, double& v) { v = m.cpu.procAvg; return m.cpu.available; }},
    {"cpu_process_p95", "pct", false, [](const MetricsSnapshot& m, double& v) { v = m.cpu.procP95; return m.cpu.available; }},
    {"cpu_system_avg", "pct", false, [](const MetricsSnapshot& m, double& v) { v = m.cpu.sysAvg; return m.cpu.available; }},
    {"cpu_busiest_core_max", "pct", false, [](const MetricsSnapshot& m, double& v) { v = m.cpu.busiestCoreMax; return m.cpu.available && m.cpu.perCoreAvailable; }},
    {"working_set_peak", "bytes", false, [](const MetricsSnapshot& m, double& v) { v = (double)m.mem.wsPeak; return m.mem.wsAvailable; }},
    {"private_bytes_peak", "bytes", false, [](const MetricsSnapshot& m, double& v) { v = (double)m.mem.privPeak; return m.mem.privAvailable; }},
  };
}
} // namespace

bool BenchProfile::passed(BenchId id) const { for (auto& s : scenariosPassed) if (s == benchIdName(id)) return true; return false; }

std::string BenchResult::toJson(int indent) const {
  json j; j["schema"] = schema; j["wall_ms"] = wallMs; j["mono_start_ns"] = monoStartNs;
  json e; e["fixture_id"] = S(env.fixtureId); e["scene_seed"] = env.sceneSeed; e["app_hash"] = S(env.appHash); e["os"] = S(env.os); e["power_plan"] = S(env.powerPlan);
  e["cpu"] = S(env.cpu); e["gpu"] = S(env.gpu); e["gpu_driver"] = S(env.gpuDriver); e["monitor_topology"] = S(env.monitorTopology); e["composition_sizes"] = S(env.compositionSizes);
  e["capture_format"] = S(env.captureFormat); e["encoder"] = S(env.encoder); e["bitrate"] = S(env.bitrate);
  json dh = json::object(), mh = json::object(), ex = json::object();
  for (auto& kv : env.dependencyHashes) dh[S(kv.first)] = S(kv.second);
  for (auto& kv : env.mediaHashes) mh[S(kv.first)] = S(kv.second);
  for (auto& kv : env.extra) ex[S(kv.first)] = sec::isSecretKeyName(kv.first) ? std::string(sec::kRedacted) : S(kv.second);
  e["dependency_hashes"] = dh; e["media_hashes"] = mh; e["extra"] = ex; j["environment"] = e;
  json p; p["warmup_s"] = nsToSec(warmupNs); p["run_s"] = nsToSec(runNs); p["runs"] = runsPerScenario; p["duration_scale"] = durationScale; p["scaled"] = scaled; j["protocol"] = p;
  j["isolation_ok"] = isolationOk; j["qualifying"] = qualifying;
  j["keys"] = {{"comparable", comparableKey}, {"hardware", hardwareKey}, {"app", appKey}, {"profile", profileHash}};
  json sc = json::array(), skipped = json::array();
  for (auto& s : scenarios) {
    if (s.status == "skipped") { skipped.push_back({{"scenario", benchIdName(s.id)}, {"reason", S(s.reason)}}); }
    json x; x["scenario"] = benchIdName(s.id); x["workload"] = benchIdDescription(s.id); x["status"] = s.status; x["reason"] = S(s.reason);
    x["warmup"] = {{"seconds", s.warmupSeconds}, {"fps", s.warmupFps}, {"frames", s.warmupFrames}};
    json runs = json::array(); for (auto& r : s.runs) runs.push_back(mj(r)); x["runs"] = runs;
    json ag = json::array(); for (auto& a : s.aggs) ag.push_back(a.available ? json({{"metric", a.name}, {"unit", a.unit}, {"median", a.median}, {"worst", a.worst}}) : json({{"metric", a.name}, {"unit", a.unit}, {"available", false}}));
    x["summary"] = ag;
    if (s.gatesEvaluated) { json g = json::array(); for (auto& r : s.runGates) g.push_back(gj(r)); x["gates"] = {{"passed", s.gatesPassed}, {"per_run", g}}; }
    x["isolation"] = {{"violations", s.isolationViolations}, {"events", s.isolationEvents}, {"lighting_datagrams_discarded", s.sinkDatagrams}, {"record_bytes_discarded", s.sinkRecordBytes}, {"broadcast_packets_discarded", s.sinkPackets}};
    sc.push_back(x);
  }
  j["scenarios"] = sc; j["skipped"] = skipped;
  if (ben06.present) j["ben06"] = {{"admission", ben06.admission}, {"recommended_tier", ben06.tier},
    {"cpu_headroom_pct", ben06.cpuHeadroomAvailable ? json(ben06.cpuHeadroomPct) : json("unavailable")}, {"frame_time_headroom_pct", ben06.frameHeadroomAvailable ? json(ben06.frameHeadroomPct) : json("unavailable")}};
  return j.dump(indent, ' ', false, json::error_handler_t::replace);
}

BenchProfile BenchResult::toProfile() const {
  BenchProfile p; p.profileHash = profileHash; p.comparableKey = comparableKey; p.hardwareKey = hardwareKey; p.appKey = appKey; p.wallMs = wallMs; p.qualifying = qualifying;
  p.tier = ben06.present ? ben06.tier : (scaled ? "untested (scaled run)" : "unverified");
  p.cpuHeadroomAvailable = ben06.cpuHeadroomAvailable; p.cpuHeadroomPct = ben06.cpuHeadroomPct; p.frameHeadroomAvailable = ben06.frameHeadroomAvailable; p.frameHeadroomPct = ben06.frameHeadroomPct;
  bool anyGate = false, allGate = true;
  for (auto& s : scenarios) {
    if (s.status != "completed") continue;
    if (s.gatesEvaluated) { anyGate = true; allGate = allGate && s.gatesPassed; }
    if ((!s.gatesEvaluated || s.gatesPassed) && isolationOk && !scaled) p.scenariosPassed.push_back(benchIdName(s.id));
    for (auto& r : s.runs) p.wsPeakBytes = std::max(p.wsPeakBytes, r.mem.wsPeak);
  }
  p.gatesPassed = anyGate && allGate; return p;
}

std::string BenchProfile::toJson() const {
  json j; j["profile_hash"] = profileHash; j["comparable_key"] = comparableKey; j["hardware_key"] = hardwareKey; j["app_key"] = appKey; j["tier"] = tier; j["wall_ms"] = wallMs;
  j["qualifying"] = qualifying; j["gates_passed"] = gatesPassed; j["cpu_headroom_available"] = cpuHeadroomAvailable; j["cpu_headroom_pct"] = cpuHeadroomPct;
  j["frame_headroom_available"] = frameHeadroomAvailable; j["frame_headroom_pct"] = frameHeadroomPct; j["ws_peak_bytes"] = wsPeakBytes; j["scenarios_passed"] = scenariosPassed;
  return j.dump();
}
static bool profileFromJson(const json& j, BenchProfile& o) {
  if (!j.is_object()) return false;
  try {
    o.profileHash = j.at("profile_hash"); o.comparableKey = j.at("comparable_key"); o.hardwareKey = j.at("hardware_key"); o.appKey = j.at("app_key"); o.tier = j.at("tier");
    o.wallMs = j.at("wall_ms"); o.qualifying = j.at("qualifying"); o.gatesPassed = j.at("gates_passed");
    o.cpuHeadroomAvailable = j.at("cpu_headroom_available"); o.cpuHeadroomPct = j.at("cpu_headroom_pct");
    o.frameHeadroomAvailable = j.at("frame_headroom_available"); o.frameHeadroomPct = j.at("frame_headroom_pct");
    o.wsPeakBytes = j.at("ws_peak_bytes"); o.scenariosPassed = j.at("scenarios_passed").get<std::vector<std::string>>();
  } catch (...) { return false; }
  return true;
}
bool BenchProfile::fromJson(const std::string& s, BenchProfile& out) { json j = json::parse(s, nullptr, false); if (j.is_discarded()) return false; BenchProfile t; if (!profileFromJson(j, t)) return false; out = t; return true; }

void BenchHistory::add(const BenchProfile& p) { v_.push_back(p); if (v_.size() > 16) v_.erase(v_.begin()); }
const BenchProfile* BenchHistory::findMatching(const std::string& hw, const std::string& app, int64_t now, int64_t maxAge) const {
  for (size_t i = v_.size(); i-- > 0;) { auto& p = v_[i]; if (p.hardwareKey == hw && p.appKey == app && now - p.wallMs <= maxAge && now >= p.wallMs) return &p; }
  return nullptr;
}
std::string BenchHistory::toJson() const { json a = json::array(); for (auto& p : v_) a.push_back(json::parse(p.toJson())); return a.dump(); }
bool BenchHistory::fromJson(const std::string& s) {
  json a = json::parse(s, nullptr, false); if (a.is_discarded() || !a.is_array() || a.size() > 64) return false;
  std::vector<BenchProfile> v; for (auto& e : a) { BenchProfile p; if (!profileFromJson(e, p)) return false; v.push_back(p); }
  if (v.size() > 16) v.erase(v.begin(), v.end() - 16);
  v_ = v; return true;
}

BenchmarkRunner::BenchmarkRunner(IClock& clk, BenchConfig cfg, ICpuCounters* cpu, IMemCounters* mem) : clk_(clk), cfg_(cfg), cpu_(cpu), mem_(mem), sinks_(new BenchmarkSinks) {
  if (cfg_.runs < 1) cfg_.runs = 1;
  if (cfg_.runs > 10) cfg_.runs = 10;
  if (!(cfg_.durationScale > 0)) cfg_.durationScale = 1.0;
}

BenchResult BenchmarkRunner::run(const BenchEnvironment& env) {
  BenchResult R; R.env = env; R.wallMs = clk_.wallUnixMs(); R.monoStartNs = clk_.nowNs();
  R.durationScale = cfg_.durationScale; R.scaled = cfg_.durationScale != 1.0 || cfg_.warmupNs != 15 * kNsPerSec || cfg_.runNs != 60 * kNsPerSec || cfg_.runs != 3;
  R.warmupNs = std::max<Ns>(kNsPerMs, (Ns)((double)cfg_.warmupNs * cfg_.durationScale)); R.runNs = std::max<Ns>(kNsPerMs, (Ns)((double)cfg_.runNs * cfg_.durationScale)); R.runsPerScenario = cfg_.runs;
  MetricsConfig mc; mc.targetFps = cfg_.limits.targetFps; MetricsRecorder metrics(clk_, mc);
  bool allRan = true;
  for (int bi = 0; bi < (int)BenchId::Count_; ++bi) {
    BenchId id = (BenchId)bi; if (!cfg_.enabled.count(id)) continue;
    ScenarioResult sr; sr.id = id;
    auto it = fns_.find(id);
    if (it == fns_.end() || !it->second) { sr.status = "skipped"; sr.reason = "no scenario implementation supplied"; allRan = false; R.scenarios.push_back(sr); continue; }
    sinks_.reset(new BenchmarkSinks);            // fresh disposable sinks per scenario
    auto makeCtx = [&](int run, bool warm, Ns dur) {
      ScenarioContext c{id, run, warm, clk_.nowNs(), clk_.nowNs() + dur, clk_, metrics, *sinks_, cpu_, mem_, env.sceneSeed}; return c; };
    // warm-up (separate record)
    metrics.reset(); ScenarioContext wc = makeCtx(-1, true, R.warmupNs); ScenarioOutcome wo = it->second(wc);
    MetricsSnapshot ws = metrics.snapshot(); sr.warmupSeconds = ws.durationSec; sr.warmupFps = ws.presentFps; sr.warmupFrames = ws.framesPresented;
    ScenarioOutcome out = wo;
    for (int r = 0; r < cfg_.runs && out.kind == ScenarioOutcome::Ran && !sinks_->guard.tripped(); ++r) {
      metrics.reset(); ScenarioContext c = makeCtx(r, false, R.runNs); out = it->second(c);
      if (out.kind == ScenarioOutcome::Ran) sr.runs.push_back(metrics.snapshot());
    }
    sr.isolationViolations = sinks_->guard.count(); sr.isolationEvents = sinks_->guard.events();
    sr.sinkDatagrams = sinks_->lighting.datagramsSent(); sr.sinkRecordBytes = sinks_->recording.bytes(); sr.sinkPackets = sinks_->broadcast.packets();
    if (sr.isolationViolations) { sr.status = "failed"; sr.reason = "isolation violation: benchmark attempted network transmission (REQ-P02)"; R.isolationOk = false; allRan = false; }
    else if (out.kind == ScenarioOutcome::Skipped) { sr.status = "skipped"; sr.reason = out.reason; sr.runs.clear(); allRan = false; }
    else if (out.kind == ScenarioOutcome::Failed) { sr.status = "failed"; sr.reason = out.reason; allRan = false; }
    else sr.status = "completed";
    if (sr.status == "completed") {
      for (auto& ex : extractors()) {
        Agg a; a.name = ex.name; a.unit = ex.unit; std::vector<double> vals; bool av = true;
        for (auto& m : sr.runs) { double v = 0; if (ex.get(m, v)) vals.push_back(v); else av = false; }
        if (av && !vals.empty()) { a.available = true; a.median = median(vals); a.worst = ex.higherIsBetter ? *std::min_element(vals.begin(), vals.end()) : *std::max_element(vals.begin(), vals.end()); }
        sr.aggs.push_back(a);
      }
      if (cfg_.gated.count(id)) {
        sr.gatesEvaluated = true; sr.gatesPassed = true;
        GateContext gc; gc.scaledRun = R.scaled; gc.isBaseFixture = cfg_.isBaseFixture; gc.captureCounterAvailable = true;
        gc.requireLatency = id == BenchId::BEN06 || id == BenchId::BEN01;
        for (auto& m : sr.runs) { GateReport g = evaluateBaseGates(m, cfg_.limits, gc); sr.gatesPassed = sr.gatesPassed && g.passed(); sr.runGates.push_back(g); }
      }
    }
    R.scenarios.push_back(sr);
  }
  // BEN-06 headroom / admission / tier
  for (auto& s : R.scenarios) if (s.id == BenchId::BEN06) {
    R.ben06.present = true;
    if (s.status != "completed") { R.ben06.admission = "untested"; R.ben06.tier = "untested"; break; }
    const double period = 1000.0 / cfg_.limits.targetFps;
    double cpuWorst = 0, renderWorst = 0; bool cpuAv = true, rAv = true;
    for (auto& m : s.runs) { if (m.cpu.available) cpuWorst = std::max(cpuWorst, m.cpu.procAvg); else cpuAv = false; if (m.renderTimeMs.available) renderWorst = std::max(renderWorst, m.renderTimeMs.p95); else rAv = false; }
    R.ben06.cpuHeadroomAvailable = cpuAv; R.ben06.cpuHeadroomPct = cfg_.limits.cpuAvgMaxPct - cpuWorst;
    R.ben06.frameHeadroomAvailable = rAv; R.ben06.frameHeadroomPct = (cfg_.limits.renderP95MaxFractionOfPeriod * period - renderWorst) / period * 100.0;
    bool complete = !s.runGates.empty(); for (auto& g : s.runGates) complete = complete && g.complete();
    if (R.scaled) { R.ben06.admission = "untested"; R.ben06.tier = "untested (scaled run)"; }
    else if (s.gatesPassed) { R.ben06.admission = "admitted"; R.ben06.tier = "BASE"; }
    else if (!complete) { R.ben06.admission = "untested"; R.ben06.tier = "unverified"; }
    else { R.ben06.admission = "not_admitted"; R.ben06.tier = "below BASE"; }
  }
  R.qualifying = !R.scaled && R.isolationOk && allRan;
  std::string cmp = R.schema + "|" + env.fixtureId + "|" + std::to_string(env.sceneSeed) + "|" + env.compositionSizes + "|" + env.captureFormat + "|" + env.encoder + "|" + env.bitrate + "|" +
                    std::to_string(R.warmupNs) + "|" + std::to_string(R.runNs) + "|" + std::to_string(R.runsPerScenario);
  for (auto& kv : env.mediaHashes) cmp += "|" + kv.first + "=" + kv.second;
  for (auto& s : R.scenarios) cmp += std::string("|") + benchIdName(s.id);
  std::string hw = env.os + "|" + env.cpu + "|" + env.gpu + "|" + env.gpuDriver + "|" + env.monitorTopology + "|" + env.powerPlan;
  std::string app = env.appHash; for (auto& kv : env.dependencyHashes) app += "|" + kv.first + "=" + kv.second;
  R.comparableKey = fnv1a64(cmp); R.hardwareKey = fnv1a64(hw); R.appKey = fnv1a64(app); R.profileHash = fnv1a64(R.comparableKey + R.hardwareKey + R.appKey);
  return R;
}

} // namespace ss
