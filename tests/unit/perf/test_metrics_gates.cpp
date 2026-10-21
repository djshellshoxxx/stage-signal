#include "doctest/doctest.h"
#include "perf_fixtures.hpp"
#include "ss/perf/gates.hpp"

using namespace perftest;

namespace {
const GateResult* gate(const GateReport& r, const std::string& id) {
    for (const GateResult& g : r.gates) if (g.id == id) return &g;
    return nullptr;
}
GateReport evalRun(const RunShape& s, bool latency = true, bool overruns = true, bool lowered = false) {
    MetricSet m = synthRun(s);
    GateInput in; in.metrics = &m; in.presentationRateHz = s.rate; in.gateLatency = latency; in.gateCaptureOverruns = overruns;
    in.sceneFpsLoweredByShedding = lowered;
    return evaluateBaseGates(GateThresholds{}, in);
}
}  // namespace

TEST_CASE("REQ-P01: sample window nearest-rank percentiles are exact and bounded") {
    SampleWindow w(100);
    for (int i = 1; i <= 100; ++i) w.add(i);
    Percentiles p = w.percentiles();
    CHECK(p.available);
    CHECK(p.p50 == 50); CHECK(p.p95 == 95); CHECK(p.p99 == 99); CHECK(p.max == 100);
    // ring keeps only the newest `capacity` samples
    for (int i = 101; i <= 150; ++i) w.add(i);
    CHECK(w.size() == 100);
    CHECK(w.percentiles().max == 150);
    SampleWindow empty(8);
    CHECK_FALSE(empty.percentiles().available);
}

TEST_CASE("REQ-P01: collector reports FPS, windows, frame times and miss counters separately") {
    MetricSet m = synthRun(RunShape{});
    CHECK(m.value("fps.mean") == doctest::Approx(25.0).epsilon(0.01));
    CHECK(m.value("fps.windows.total") == 60);
    CHECK(m.value("fps.windows.share") >= 0.98);
    CHECK(m.value("interval.p50.ms") == doctest::Approx(40.0));
    CHECK(m.value("interval.p99.ms") == doctest::Approx(40.0));
    CHECK(m.value("render.p95.ms") == doctest::Approx(20.0));
    CHECK(m.has("miss.render")); CHECK(m.has("miss.presentation")); CHECK(m.has("miss.decoder")); CHECK(m.has("miss.encoder"));

    RunRecorder rec(25.0);
    rec.begin(0);
    rec.miss(MissKind::Decoder, 3); rec.miss(MissKind::Encoder, 2); rec.miss(MissKind::Render, 1);
    rec.frame(100 * kNsPerMs, 90 * kNsPerMs);   // 90 ms render exceeds the 40 ms period: counted as a render miss
    MetricSet r = rec.finish(2 * kNsPerSec);
    CHECK(r.value("miss.decoder") == 3);
    CHECK(r.value("miss.encoder") == 2);
    CHECK(r.value("miss.render") == 2);
    CHECK(r.value("miss.presentation") == 0);
}

TEST_CASE("REQ-P01: seconds with no frames count as failed windows") {
    RunRecorder rec(25.0);
    rec.begin(0);
    for (int i = 0; i < 25; ++i) rec.frame(i * 40 * kNsPerMs, kNsPerMs);          // second 0 full
    for (int i = 0; i < 25; ++i) rec.frame(5 * kNsPerSec + i * 40 * kNsPerMs, kNsPerMs);   // seconds 1..4 empty, 5 full
    MetricSet m = rec.finish(6 * kNsPerSec);
    CHECK(m.value("fps.windows.total") == 6);
    CHECK(m.value("fps.windows.meeting") == 2);
}

TEST_CASE("REQ-P01: CPU is normalised across logical processors and missing counters are unavailable") {
    CpuSample c = normaliseCpu({100.0, 0.0, 50.0, 50.0}, 1 * kNsPerSec, 1 * kNsPerSec, 4);
    CHECK(c.available);
    CHECK(c.systemPct == doctest::Approx(50.0));
    CHECK(c.busiestCorePct == doctest::Approx(100.0));
    CHECK(c.processPct == doctest::Approx(25.0));   // one core-second of CPU on four logical processors
    CHECK_FALSE(normaliseCpu({}, 0, kNsPerSec, 4).available);

    RunShape s; s.cpuAvailable = false; s.memAvailable = false; s.recordDeadlines = false; s.uiAck = -1; s.scene = -1; s.onset = -1;
    s.overrunsMeasured = false;
    MetricSet m = synthRun(s);
    for (const char* k : {"cpu.process.avg", "cpu.system.p95", "mem.ws.peak", "mem.commit.steady", "mem.free.min",
                          "miss.presentation.fraction", "latency.ui.p95.ms", "capture.overruns"}) {
        INFO(k);
        CHECK(m.has(k));
        CHECK_FALSE(m.available(k));
    }
}

TEST_CASE("REQ-P06: a healthy BASE run passes every gate") {
    GateReport r = evalRun(RunShape{});
    for (const GateResult& g : r.gates) { INFO(g.id << " measured " << g.measured << " limit " << g.limit); CHECK(g.state == GateState::Pass); }
    CHECK(r.verdict == Verdict::Pass);
    CHECK(r.failed == 0);
    CHECK(r.gates.size() >= 20);
}

TEST_CASE("REQ-P06: rate and window gates fail when frames are missing") {
    RunShape s; s.rate = 25.0;
    MetricSet m = synthRun(s);
    // 23 FPS delivered against a 25 FPS target: 92% < 95%, and 23 < 24 frames in nearly every window
    RunRecorder rec(25.0);
    rec.begin(0);
    for (Ns t = 43 * kNsPerMs; t < 60 * kNsPerSec; t += 43478260) rec.frame(t, 10 * kNsPerMs);
    MetricSet slow = rec.finish(60 * kNsPerSec);
    GateInput in; in.metrics = &slow; in.presentationRateHz = 25.0;
    GateReport r = evaluateBaseGates(GateThresholds{}, in);
    CHECK(gate(r, "G-FPS-RATE")->state == GateState::Fail);
    CHECK(gate(r, "G-FPS-WINDOWS")->state == GateState::Fail);
    CHECK(r.verdict == Verdict::Fail);
    (void)m;
}

TEST_CASE("REQ-P06: threshold boundaries are exact") {
    GateThresholds t;
    MetricSet m;
    auto run = [&](double render95, double interval95, double interval99, double cpuAvg, double cpu95) {
        m = MetricSet{};
        m.set("fps.mean", 25); m.set("render.p95.ms", render95); m.set("miss.presentation.fraction", 0.01);
        m.set("fps.windows.share", 0.95); m.set("interval.p95.ms", interval95); m.set("interval.p99.ms", interval99);
        m.set("interval.max.ms", 200); m.set("render.max.ms", 100);
        m.set("mem.ws.steady", 96.0 * kMiB); m.set("mem.ws.peak", 128.0 * kMiB); m.set("mem.commit.steady", 96.0 * kMiB);
        m.set("mem.commit.peak", 128.0 * kMiB); m.set("mem.host.peak", 64.0 * kMiB); m.set("mem.free.min", 32.0 * kMiB);
        m.set("cpu.process.avg", cpuAvg); m.set("cpu.process.p95", cpu95);
        GateInput in; in.metrics = &m; in.gateLatency = false;
        return evaluateBaseGates(t, in);
    };
    GateReport edge = run(32.0, 50.0, 100.0, 65.0, 80.0);      // every value exactly on its limit
    CHECK(edge.verdict == Verdict::Pass);
    CHECK(run(32.01, 50, 100, 65, 80).verdict == Verdict::Fail);       // render p95 > 80% of 40 ms
    CHECK(run(32, 50.01, 100, 65, 80).verdict == Verdict::Fail);
    CHECK(run(32, 50, 100.01, 65, 80).verdict == Verdict::Fail);
    CHECK(run(32, 50, 100, 65.01, 80).verdict == Verdict::Fail);
    CHECK(run(32, 50, 100, 65, 80.01).verdict == Verdict::Fail);
}

TEST_CASE("REQ-P06: stall over 200 ms fails even when everything else is fine") {
    RunShape s; s.stallAtSec = 30; s.stallLen = 260 * kNsPerMs;
    GateReport r = evalRun(s);
    CHECK(gate(r, "G-STALL")->state == GateState::Fail);
    CHECK(gate(r, "G-STALL")->measured > 200.0);
    CHECK(r.verdict == Verdict::Fail);
    RunShape ok; ok.stallAtSec = 30; ok.stallLen = 100 * kNsPerMs;
    CHECK(gate(evalRun(ok), "G-STALL")->state == GateState::Pass);
}

TEST_CASE("REQ-P06: memory gates use 96/128/64 MiB and 32 MiB free") {
    RunShape s;
    s.ws = 97 * kMiB; CHECK(gate(evalRun(s), "G-WS-STEADY")->state == GateState::Fail);
    s = RunShape{}; s.commit = 129 * kMiB; CHECK(gate(evalRun(s), "G-COMMIT-PEAK")->state == GateState::Fail);
    s = RunShape{}; s.host = 65 * kMiB; CHECK(gate(evalRun(s), "G-HOST-MEM")->state == GateState::Fail);
    s = RunShape{}; s.freeBytes = 31 * kMiB; CHECK(gate(evalRun(s), "G-FREE-MEM")->state == GateState::Fail);
    s = RunShape{}; s.freeBytes = 32 * kMiB; CHECK(gate(evalRun(s), "G-FREE-MEM")->state == GateState::Pass);
}

TEST_CASE("REQ-P06: response gates need measurements; capture overruns must be zero") {
    RunShape s; s.uiAck = 101 * kNsPerMs;
    CHECK(gate(evalRun(s), "G-UI-ACK")->state == GateState::Fail);
    s = RunShape{}; s.scene = 151 * kNsPerMs;
    CHECK(gate(evalRun(s), "G-SCENE-RESPONSE")->state == GateState::Fail);
    s = RunShape{}; s.onset = 151 * kNsPerMs;
    CHECK(gate(evalRun(s), "G-ONSET-RESPONSE")->state == GateState::Fail);
    s = RunShape{}; s.uiAck = -1;
    GateReport r = evalRun(s);
    CHECK(gate(r, "G-UI-ACK")->state == GateState::Unavailable);
    CHECK(r.verdict == Verdict::Incomplete);        // never a pass with a missing counter
    CHECK(evalRun(RunShape{}, false, false).gates.size() < evalRun(RunShape{}).gates.size());

    RunRecorder rec(25.0); rec.begin(0); rec.captureOverrun(2);
    MetricSet m = rec.finish(kNsPerSec);
    GateInput in; in.metrics = &m; in.gateCaptureOverruns = true; in.gateLatency = false;
    CHECK(gate(evaluateBaseGates(GateThresholds{}, in), "G-CAPTURE-OVERRUNS")->state == GateState::Fail);
}

TEST_CASE("REQ-P06: lowering the BASE 25 FPS target never passes the gates") {
    // Even a flawless run at 20 FPS fails: shedding is recovery, not a way to pass.
    RunShape s; s.rate = 20.0;
    GateReport r20 = evalRun(s);
    CHECK(gate(r20, "G-TARGET-FPS")->state == GateState::Fail);
    CHECK(r20.verdict == Verdict::Fail);
    GateReport shed = evalRun(RunShape{}, true, true, /*lowered=*/true);
    CHECK(gate(shed, "G-TARGET-FPS")->state == GateState::Fail);
    CHECK(shed.verdict == Verdict::Fail);
}

TEST_CASE("REQ-P06: missed deadlines above 1% fail") {
    RunShape s; s.missEvery = 50;     // 2%
    CHECK(gate(evalRun(s), "G-MISSED")->state == GateState::Fail);
    s.missEvery = 200;                // 0.5%
    CHECK(gate(evalRun(s), "G-MISSED")->state == GateState::Pass);
}
