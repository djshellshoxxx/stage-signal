#include "ss/perf/gates.hpp"
#include <algorithm>

namespace ss::perf {

const char* gateStateName(GateState s) {
    switch (s) { case GateState::Pass: return "pass"; case GateState::Fail: return "fail"; case GateState::Unavailable: return "unavailable"; }
    return "unavailable";
}
const char* verdictName(Verdict v) {
    switch (v) { case Verdict::Pass: return "pass"; case Verdict::Fail: return "fail"; case Verdict::Incomplete: return "incomplete"; }
    return "incomplete";
}

namespace {
constexpr double kMiBd = 1024.0 * 1024.0;

struct Builder {
    const MetricSet& m;
    GateReport rep;

    void add(const char* id, const char* desc, const char* metric, double limit, bool upper, const char* unit,
             const char* req, double scale = 1.0) {
        GateResult g;
        g.id = id; g.description = desc; g.limit = limit; g.upperBound = upper; g.unit = unit; g.req = req;
        if (m.available(metric)) {
            g.measured = m.value(metric) * scale;
            g.state = (upper ? g.measured <= limit : g.measured >= limit) ? GateState::Pass : GateState::Fail;
        }
        push(std::move(g));
    }
    void push(GateResult g) {
        switch (g.state) {
            case GateState::Pass: ++rep.passed; break;
            case GateState::Fail: ++rep.failed; break;
            case GateState::Unavailable: ++rep.unavailable; break;
        }
        rep.gates.push_back(std::move(g));
    }
};
}  // namespace

GateReport evaluateBaseGates(const GateThresholds& t, const GateInput& in) {
    static const MetricSet kEmpty;
    const MetricSet& m = in.metrics ? *in.metrics : kEmpty;
    Builder b{m, {}};
    const double rate = in.presentationRateHz > 0 ? in.presentationRateHz : 1.0;
    const double periodMs = 1000.0 / rate;

    // The BASE 25 FPS target may never be lowered to pass: either the configured rate is below the BASE
    // target or the run itself shed scene FPS.
    {
        GateResult g;
        g.id = "G-TARGET-FPS"; g.description = "BASE target presentation rate is not lowered to pass";
        g.req = "REQ-P06"; g.unit = "fps"; g.upperBound = false;
        g.measured = rate; g.limit = t.baseTargetFps;
        g.state = (rate + 1e-9 >= t.baseTargetFps && !in.sceneFpsLoweredByShedding) ? GateState::Pass : GateState::Fail;
        b.push(std::move(g));
    }
    // Rate: >= 95% of configured presentation rate.
    b.add("G-FPS-RATE", "Mean presented FPS >= 95% of presentation rate", "fps.mean", t.minRateFraction * rate, false, "fps", "section 7");
    b.add("G-RENDER-P95", "p95 render time <= 80% of frame period", "render.p95.ms", t.renderP95OfPeriod * periodMs, true, "ms", "section 7");
    b.add("G-MISSED", "Missed presentation deadlines <= 1%", "miss.presentation.fraction", t.maxMissedFraction, true, "fraction", "section 7");
    b.add("G-FPS-WINDOWS", "At least (rate-1) frames in 95% of 1 s windows", "fps.windows.share", t.minWindowShare, false, "fraction", "REQ-P06");
    b.add("G-INTERVAL-P95", "p95 presentation interval <= 50 ms", "interval.p95.ms", t.intervalP95Ms, true, "ms", "REQ-P06");
    b.add("G-INTERVAL-P99", "p99 presentation interval <= 100 ms", "interval.p99.ms", t.intervalP99Ms, true, "ms", "REQ-P06");

    // No stall > 200 ms: worst of the longest interval and the longest render.
    {
        GateResult g;
        g.id = "G-STALL"; g.description = "No render or presentation stall > 200 ms"; g.req = "REQ-P06";
        g.unit = "ms"; g.limit = t.stallMs; g.upperBound = true;
        if (m.available("interval.max.ms") || m.available("render.max.ms")) {
            g.measured = std::max(m.value("interval.max.ms"), m.value("render.max.ms"));
            g.state = g.measured <= g.limit ? GateState::Pass : GateState::Fail;
        }
        b.push(std::move(g));
    }

    if (in.gateLatency) {
        b.add("G-UI-ACK", "UI action acknowledgment p95 <= 100 ms", "latency.ui.p95.ms", t.uiAckP95Ms, true, "ms", "REQ-P06");
        b.add("G-SCENE-RESPONSE", "Visible scene/parameter response p95 <= 150 ms", "latency.scene.p95.ms", t.sceneP95Ms, true, "ms", "REQ-P06");
        b.add("G-ONSET-RESPONSE", "Audio-to-visual onset response p95 <= 150 ms", "latency.onset.p95.ms", t.onsetP95Ms, true, "ms", "REQ-P06");
    }

    b.add("G-WS-STEADY", "Working set steady-state <= 96 MiB", "mem.ws.steady", static_cast<double>(t.steadyBytes) / kMiBd, true, "MiB", "section 7", 1.0 / kMiBd);
    b.add("G-WS-PEAK", "Working set peak <= 128 MiB", "mem.ws.peak", static_cast<double>(t.peakBytes) / kMiBd, true, "MiB", "section 7", 1.0 / kMiBd);
    b.add("G-COMMIT-STEADY", "Private commit steady-state <= 96 MiB", "mem.commit.steady", static_cast<double>(t.steadyBytes) / kMiBd, true, "MiB", "section 7", 1.0 / kMiBd);
    b.add("G-COMMIT-PEAK", "Private commit peak <= 128 MiB", "mem.commit.peak", static_cast<double>(t.peakBytes) / kMiBd, true, "MiB", "section 7", 1.0 / kMiBd);
    b.add("G-HOST-MEM", "Host-only working set <= 64 MiB", "mem.host.peak", static_cast<double>(t.hostBytes) / kMiBd, true, "MiB", "section 7", 1.0 / kMiBd);
    b.add("G-FREE-MEM", "At least 32 MiB physical memory stays available", "mem.free.min", static_cast<double>(t.minFreeBytes) / kMiBd, false, "MiB", "section 7", 1.0 / kMiBd);
    b.add("G-CPU-AVG", "Average process CPU <= 65%", "cpu.process.avg", t.avgCpuPct, true, "%", "section 7");
    b.add("G-CPU-P95", "p95 process CPU (1 s windows) <= 80%", "cpu.process.p95", t.p95CpuPct, true, "%", "section 7");

    if (in.gateCaptureOverruns)
        b.add("G-CAPTURE-OVERRUNS", "Zero capture overruns", "capture.overruns", 0.0, true, "count", "REQ-P06");

    b.rep.verdict = b.rep.failed > 0 ? Verdict::Fail : (b.rep.unavailable > 0 ? Verdict::Incomplete : Verdict::Pass);
    return std::move(b.rep);
}

}  // namespace ss::perf
