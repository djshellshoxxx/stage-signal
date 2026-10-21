#include "ss/perf/benchmark.hpp"
#include <algorithm>
#include <cmath>
#include "nlohmann/json.hpp"
#include "ss/base/sha256.hpp"
#include "ss/perf/limits.hpp"

namespace ss::perf {

using nlohmann::json;

const char* sinkKindName(SinkKind k) {
    switch (k) {
        case SinkKind::None: return "none";
        case SinkKind::Mock: return "mock";
        case SinkKind::DisposableFile: return "disposable-file";
        case SinkKind::LocalDiscard: return "local-discard";
        case SinkKind::LiveOutput: return "live-output";
        case SinkKind::LiveRecording: return "live-recording";
        case SinkKind::LiveLighting: return "live-lighting";
        case SinkKind::Broadcast: return "broadcast";
    }
    return "unknown";
}

bool isLoopbackEndpoint(const std::string& endpoint) {
    std::string e = endpoint;
    if (e.empty()) return true;
    const auto scheme = e.find("://");
    if (scheme != std::string::npos) e = e.substr(scheme + 3);
    const auto slash = e.find('/');
    if (slash != std::string::npos) e = e.substr(0, slash);
    const auto at = e.find('@');
    if (at != std::string::npos) return false;     // credentials in a URL: not a plain local receiver
    std::string host = e;
    if (!host.empty() && host[0] == '[') {
        const auto close = host.find(']');
        if (close == std::string::npos) return false;
        host = host.substr(1, close - 1);
    } else {
        const auto first = host.find(':'), last = host.rfind(':');
        if (first != std::string::npos && first == last) host = host.substr(0, first);   // host:port
    }
    std::string low;
    for (char c : host) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (low == "localhost" || low == "::1") return true;
    // dotted quad 127.a.b.c with each part 0..255
    int parts = 0, value = -1, first = -1;
    for (std::size_t i = 0; i <= low.size(); ++i) {
        if (i == low.size() || low[i] == '.') {
            if (value < 0 || value > 255) return false;
            if (parts == 0) first = value;
            ++parts; value = -1;
        } else if (low[i] >= '0' && low[i] <= '9') {
            value = (value < 0 ? 0 : value) * 10 + (low[i] - '0');
            if (value > 255) return false;
        } else {
            return false;
        }
    }
    return parts == 4 && first == 127;
}

bool sinkAllowed(const SinkDecl& s) {
    switch (s.kind) {
        case SinkKind::None:
        case SinkKind::Mock:
        case SinkKind::DisposableFile: return true;
        case SinkKind::LocalDiscard: return isLoopbackEndpoint(s.endpoint);
        default: return false;
    }
}

const std::vector<ScenarioDescriptor>& scenarios() {
    static const std::vector<ScenarioDescriptor> kScenarios = [] {
        std::vector<ScenarioDescriptor> v;
        ScenarioDescriptor s;
        s = {}; s.id = "BEN-01"; s.name = "Basic scenes";
        s.workload = "Basic scenes + audio bands + text";
        s.evidence = {"Present FPS", "p50/p95/p99 frame time", "input/output latency"};
        s.gateLatency = true; s.allowedSinks = {SinkKind::None, SinkKind::Mock};
        v.push_back(s);
        s = {}; s.id = "BEN-02"; s.name = "Duplicate and independent outputs";
        s.workload = "Duplicate and independent outputs"; s.multiOutput = true;
        s.evidence = {"Per-output missed deadlines", "total render cost"};
        s.allowedSinks = {SinkKind::None, SinkKind::Mock};
        v.push_back(s);
        s = {}; s.id = "BEN-03"; s.name = "Camera, GIF and video";
        s.workload = "Camera treatments + GIF + one video clip"; s.needsCamera = true;
        s.evidence = {"Decode drops", "upload/convert cost", "memory peaks"};
        s.allowedSinks = {SinkKind::None, SinkKind::Mock};
        v.push_back(s);
        s = {}; s.id = "BEN-04"; s.name = "Program encode to disposable file";
        s.workload = "Program encode to disposable local test file"; s.needsEncoder = true;
        s.evidence = {"Encoded FPS", "A/V sync", "write rate", "queue occupancy"};
        s.allowedSinks = {SinkKind::None, SinkKind::Mock, SinkKind::DisposableFile};
        v.push_back(s);
        s = {}; s.id = "BEN-05"; s.name = "Broadcast to local discard";
        s.workload = "Broadcast encoder/muxer to local discard/test receiver, never remote endpoint";
        s.needsEncoder = true; s.needsBroadcastEncoder = true;
        s.evidence = {"Encode cost", "packets", "bounded sink behavior"};
        s.allowedSinks = {SinkKind::None, SinkKind::Mock, SinkKind::LocalDiscard};
        v.push_back(s);
        s = {}; s.id = "BEN-06"; s.name = "Combined configuration";
        s.workload = "Selected combined performance configuration"; s.multiOutput = true;
        s.gateLatency = true; s.gateCaptureOverruns = true;
        s.evidence = {"Headroom", "admission outcome", "recommended tier"};
        s.allowedSinks = {SinkKind::None, SinkKind::Mock, SinkKind::DisposableFile, SinkKind::LocalDiscard};
        v.push_back(s);
        return v;
    }();
    return kScenarios;
}

const ScenarioDescriptor* findScenario(const std::string& id) {
    for (const ScenarioDescriptor& s : scenarios()) if (s.id == id) return &s;
    return nullptr;
}

// ---- aggregation ---------------------------------------------------------------------------
void aggregateRuns(const std::vector<MetricSet>& runs, MetricSet* median, MetricSet* worst) {
    *median = MetricSet{};
    *worst = MetricSet{};
    std::vector<std::string> names;
    for (const MetricSet& r : runs)
        for (const Metric& m : r.all())
            if (std::find(names.begin(), names.end(), m.name) == names.end()) names.push_back(m.name);
    for (const std::string& n : names) {
        std::vector<double> vals;
        bool allAvail = true;
        for (const MetricSet& r : runs) {
            const Metric* m = r.find(n);
            if (!m || !m->available) { allAvail = false; break; }
            vals.push_back(m->value);
        }
        if (!allAvail || vals.empty()) { median->setUnavailable(n); worst->setUnavailable(n); continue; }
        std::sort(vals.begin(), vals.end());
        median->set(n, vals[(vals.size() - 1) / 2]);
        worst->set(n, metricHigherIsBetter(n) ? vals.front() : vals.back());
    }
}

// ---- runner --------------------------------------------------------------------------------
Status BenchmarkRunner::validate(const ScenarioDescriptor& s, const IBenchWorkload& w) const {
    if (cfg_.runs < 1 || cfg_.runs > 10 || cfg_.runNs <= 0 || cfg_.warmupNs < 0 || cfg_.tickNs <= 0 ||
        cfg_.runNs > 3600 * kNsPerSec || cfg_.tickNs > cfg_.runNs)
        return Status(makeError(ErrorCategory::InvalidArgument, "benchmark runner config out of range"));
    if (!(w.presentationRateHz() > 0.0 && w.presentationRateHz() <= 240.0))
        return Status(makeError(ErrorCategory::InvalidArgument, "workload presentation rate out of range"));
    for (const SinkDecl& d : w.sinks()) {
        if (!sinkAllowed(d))
            return Status(makeError(ErrorCategory::PermissionDenied,
                                    std::string("benchmark refused sink '") + sinkKindName(d.kind) +
                                        "': only mock, disposable-file and loopback discard sinks are allowed"));
        if (std::find(s.allowedSinks.begin(), s.allowedSinks.end(), d.kind) == s.allowedSinks.end())
            return Status(makeError(ErrorCategory::PermissionDenied,
                                    std::string("scenario ") + s.id + " does not allow sink '" + sinkKindName(d.kind) + "'"));
    }
    return okStatus();
}

Result<ScenarioReport> BenchmarkRunner::run(const ScenarioDescriptor& s, IBenchWorkload& w, std::uint64_t seed) {
    Status v = validate(s, w);
    if (!v.ok()) return v.error();
    Status prep = w.prepare(s, seed);
    if (!prep.ok()) return prep.error();

    ScenarioReport rep;
    rep.scenarioId = s.id; rep.scenarioName = s.name; rep.seed = seed;
    rep.presentationRateHz = w.presentationRateHz();

    auto phase = [&](RunRecorder& rec, Ns duration) -> bool {
        const Ns t0 = time_.nowMonotonic();
        rec.begin(t0);
        const Ns end = t0 + duration;
        for (Ns t = t0; t < end; t += cfg_.tickNs) {
            if (cfg_.cancel && cfg_.cancel->load(std::memory_order_relaxed)) return false;
            time_.waitUntil(t);
            w.step(t, rec);
        }
        time_.waitUntil(end);
        return true;
    };

    {   // warm-up: recorded separately, never gated
        RunRecorder warm(rep.presentationRateHz);
        const Ns t0 = time_.nowMonotonic();
        if (cfg_.warmupNs > 0 && !phase(warm, cfg_.warmupNs)) { w.finish(); return makeError(ErrorCategory::Cancelled, "benchmark cancelled"); }
        rep.warmupSec = nsToSec(time_.nowMonotonic() - t0);
        rep.warmupFrames = warm.framesPresented();
    }
    for (int i = 0; i < cfg_.runs; ++i) {
        RunRecorder rec(rep.presentationRateHz);
        if (!phase(rec, cfg_.runNs)) { w.finish(); return makeError(ErrorCategory::Cancelled, "benchmark cancelled"); }
        rep.runs.push_back(rec.finish(time_.nowMonotonic()));
    }
    w.finish();
    rep.sceneRateLowered = w.sceneRateLowered();
    rep.adaptationStepAtEnd = w.adaptationStep();
    aggregateRuns(rep.runs, &rep.median, &rep.worst);

    GateInput in;
    in.presentationRateHz = rep.presentationRateHz;
    in.sceneFpsLoweredByShedding = rep.sceneRateLowered;
    in.gateLatency = s.gateLatency;
    in.gateCaptureOverruns = s.gateCaptureOverruns;
    in.metrics = &rep.worst;
    rep.gatesWorst = evaluateBaseGates(cfg_.thresholds, in);
    in.metrics = &rep.median;
    rep.gatesMedian = evaluateBaseGates(cfg_.thresholds, in);
    return rep;
}

Result<SuiteReport> BenchmarkRunner::runSuite(const BenchEnvironment& env, const std::vector<SuiteEntry>& entries,
                                              std::uint64_t seed) {
    SuiteReport out;
    out.env = env;
    for (const SuiteEntry& e : entries) {
        const ScenarioDescriptor* sd = findScenario(e.scenarioId);
        if (!sd) { out.skipped.push_back({e.scenarioId, "unknown scenario"}); continue; }
        if (!e.workload) {
            out.skipped.push_back({e.scenarioId, e.skipReason.empty() ? "no workload available" : e.skipReason});
            continue;
        }
        Result<ScenarioReport> r = run(*sd, *e.workload, seed);
        if (!r.ok()) {
            if (r.error().category == ErrorCategory::Cancelled) return r.error();
            out.skipped.push_back({e.scenarioId, r.error().detail});
            continue;
        }
        out.scenarios.push_back(std::move(r.value()));
    }
    out.capturedAtUnixMs = time_.wallUnixNs() / kNsPerMs;
    return out;
}

// ---- JSON ----------------------------------------------------------------------------------
namespace {
double r4(double v) { return std::round(v * 1e4) / 1e4; }

std::string clip(const std::string& s) { return s.size() > kMaxStringBytes ? s.substr(0, kMaxStringBytes) : s; }

template <class T>
json clipList(const std::vector<T>& v) {
    json a = json::array();
    for (std::size_t i = 0; i < v.size() && i < kMaxReportStrings; ++i) a.push_back(v[i]);
    return a;
}

json envToJson(const BenchEnvironment& e) {
    json j;
    j["appHash"] = clip(e.appHash);
    json deps = json::object();
    for (std::size_t i = 0; i < e.dependencyHashes.size() && i < kMaxReportStrings; ++i)
        deps[clip(e.dependencyHashes[i].first)] = clip(e.dependencyHashes[i].second);
    j["dependencyHashes"] = deps;
    j["os"] = clip(e.os); j["powerPlan"] = clip(e.powerPlan);
    j["cpu"] = clip(e.cpu); j["logicalProcessors"] = e.logicalProcessors;
    j["gpu"] = clip(e.gpu); j["gpuDriver"] = clip(e.gpuDriver);
    j["gpuTimingAvailable"] = e.gpuTimingAvailable;
    json mons = json::array();
    for (std::size_t i = 0; i < e.monitors.size() && i < kMaxReportStrings; ++i) {
        const MonitorInfo& m = e.monitors[i];
        mons.push_back({{"width", m.width}, {"height", m.height}, {"rotationDeg", m.rotationDeg}, {"primary", m.primary}});
    }
    j["monitors"] = mons;
    json comps = json::array();
    for (std::size_t i = 0; i < e.compositions.size() && i < kMaxReportStrings; ++i)
        comps.push_back({{"width", e.compositions[i].width}, {"height", e.compositions[i].height}});
    j["compositions"] = comps;
    j["capture"] = {{"rateHz", e.captureRateHz}, {"channels", e.captureChannels}, {"format", clip(e.captureFormat)}};
    std::vector<std::string> media;
    for (const auto& m : e.mediaHashes) media.push_back(clip(m));
    j["mediaHashes"] = clipList(media);
    j["encoder"] = {{"name", clip(e.encoder)}, {"bitrateKbps", e.bitrateKbps}};
    return j;
}

json metricsToJson(const MetricSet& m) {
    json o = json::object();
    for (const Metric& x : m.all()) o[x.name] = x.available ? json(r4(x.value)) : json(nullptr);
    return o;
}

json gatesToJson(const GateReport& g) {
    json o;
    o["verdict"] = verdictName(g.verdict);
    o["passed"] = g.passed; o["failed"] = g.failed; o["unavailable"] = g.unavailable;
    json arr = json::array();
    for (const GateResult& r : g.gates) {
        json x;
        x["id"] = r.id; x["description"] = r.description; x["state"] = gateStateName(r.state);
        x["measured"] = r.state == GateState::Unavailable ? json(nullptr) : json(r4(r.measured));
        x["limit"] = r4(r.limit); x["op"] = r.upperBound ? "<=" : ">="; x["unit"] = r.unit; x["req"] = r.req;
        arr.push_back(x);
    }
    o["gates"] = arr;
    return o;
}
}  // namespace

std::string environmentJson(const BenchEnvironment& env) { return envToJson(env).dump(); }

std::string SuiteReport::profileHash() const { return Sha256::hashHex(environmentJson(env)); }

std::string toJson(const SuiteReport& r, int indent) {
    json j;
    j["schema"] = r.schema;
    j["capturedAtUnixMs"] = r.capturedAtUnixMs;
    j["profileHash"] = r.profileHash();
    j["environment"] = envToJson(r.env);
    json sc = json::array();
    for (const ScenarioReport& s : r.scenarios) {
        json x;
        x["id"] = s.scenarioId; x["name"] = s.scenarioName; x["seed"] = s.seed;
        x["presentationRateHz"] = r4(s.presentationRateHz);
        x["warmup"] = {{"durationSec", r4(s.warmupSec)}, {"frames", s.warmupFrames}};
        json runs = json::array();
        for (std::size_t i = 0; i < s.runs.size(); ++i) runs.push_back({{"index", i + 1}, {"metrics", metricsToJson(s.runs[i])}});
        x["runs"] = runs;
        x["median"] = metricsToJson(s.median);
        x["worst"] = metricsToJson(s.worst);
        x["gatesWorst"] = gatesToJson(s.gatesWorst);
        x["gatesMedian"] = gatesToJson(s.gatesMedian);
        x["verdict"] = verdictName(s.gatesWorst.verdict);
        x["adaptationStepAtEnd"] = s.adaptationStepAtEnd;
        x["sceneRateLowered"] = s.sceneRateLowered;
        sc.push_back(x);
    }
    j["scenarios"] = sc;
    json sk = json::array();
    for (const SkippedTest& s : r.skipped) sk.push_back({{"scenario", s.scenarioId}, {"reason", clip(s.reason)}});
    j["skipped"] = sk;
    return j.dump(indent);
}

}  // namespace ss::perf
