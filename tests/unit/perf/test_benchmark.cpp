#include "doctest/doctest.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include "perf_fixtures.hpp"

using namespace perftest;

namespace {
RunnerConfig fastCfg() {
    RunnerConfig c;
    c.warmupNs = 3 * kNsPerSec; c.runNs = 10 * kNsPerSec; c.runs = 3; c.tickNs = 5 * kNsPerMs;
    return c;
}
BenchEnvironment env() {
    BenchEnvironment e;
    e.appHash = "abc123"; e.dependencyHashes = {{"juce", "h1"}, {"nlohmann", "h2"}};
    e.os = "TestOS 1"; e.powerPlan = "High performance"; e.cpu = "Test CPU 800 MHz"; e.logicalProcessors = 1;
    e.gpu = "none"; e.monitors = {{800, 600, 0, true}, {1024, 768, 0, false}};
    e.compositions = {{800, 600}}; e.captureRateHz = 48000; e.captureChannels = 2; e.captureFormat = "float32";
    e.mediaHashes = {"m1"}; e.encoder = ""; e.bitrateKbps = 0;
    return e;
}
}  // namespace

TEST_CASE("REQ-P01: scenarios BEN-01..BEN-06 are described with evidence and benign sinks only") {
    const auto& s = scenarios();
    REQUIRE(s.size() == 6);
    for (int i = 0; i < 6; ++i) {
        CHECK(s[i].id == std::string("BEN-0") + std::to_string(i + 1));
        CHECK_FALSE(s[i].evidence.empty());
        CHECK_FALSE(s[i].allowedSinks.empty());
        for (SinkKind k : s[i].allowedSinks) CHECK(sinkAllowed(SinkDecl{k, "", ""}));
    }
    CHECK(findScenario("BEN-04")->needsEncoder);
    CHECK(findScenario("BEN-05")->needsBroadcastEncoder);
    CHECK(findScenario("BEN-03")->needsCamera);
    CHECK(findScenario("BEN-06")->gateCaptureOverruns);
    CHECK(findScenario("BEN-99") == nullptr);
}

TEST_CASE("REQ-P01: runner executes warm-up then 3 runs, records warm-up separately, reports median and worst") {
    FakeBenchTime time;
    BenchmarkRunner r(time, fastCfg());
    FakeWorkload w; w.declared = {{SinkKind::Mock, "mock", ""}};
    auto res = r.run(*findScenario("BEN-01"), w, 42);
    REQUIRE(res.ok());
    const ScenarioReport& rep = res.value();
    CHECK(w.prepared); CHECK(w.finished); CHECK(w.lastSeed == 42);
    CHECK(rep.runs.size() == 3);
    CHECK(rep.warmupSec == doctest::Approx(3.0));
    CHECK(rep.warmupFrames >= 70);                     // 3 s at 25 FPS, never mixed into the runs
    for (const MetricSet& m : rep.runs) {
        CHECK(m.value("duration.s") == doctest::Approx(10.0));
        CHECK(m.value("frames.presented") >= 249);
        CHECK(m.value("frames.presented") <= 251);
    }
    CHECK(time.nowMonotonic() == 3 * kNsPerSec + 30 * kNsPerSec);
    CHECK(rep.gatesWorst.verdict == Verdict::Pass);
}

TEST_CASE("REQ-P01: median is the lower median and worst is direction-aware") {
    std::vector<MetricSet> runs(3);
    const double fps[3] = {25, 20, 24}, p95[3] = {10, 50, 30};
    for (int i = 0; i < 3; ++i) { runs[i].set("fps.mean", fps[i]); runs[i].set("interval.p95.ms", p95[i]); }
    MetricSet med, worst;
    aggregateRuns(runs, &med, &worst);
    CHECK(med.value("fps.mean") == 24);
    CHECK(worst.value("fps.mean") == 20);               // higher is better => minimum is the worst
    CHECK(med.value("interval.p95.ms") == 30);
    CHECK(worst.value("interval.p95.ms") == 50);        // lower is better => maximum is the worst
    runs[1].setUnavailable("interval.p95.ms");
    aggregateRuns(runs, &med, &worst);
    CHECK_FALSE(med.available("interval.p95.ms"));      // one missing run => the aggregate is not trusted
    CHECK_FALSE(worst.available("interval.p95.ms"));
}

TEST_CASE("REQ-P01: worst run drives the gate verdict") {
    FakeBenchTime time;
    RunnerConfig cfg = fastCfg();
    BenchmarkRunner r(time, cfg);
    // A workload whose CPU degrades over the three runs: only the worst run exceeds the 65% limit.
    struct Degrading : FakeWorkload {
        void step(Ns now, RunRecorder& rec) override {
            const Ns phaseIdx = (now - 3 * kNsPerSec) / (10 * kNsPerSec);
            shape.cpuPct = phaseIdx >= 2 ? 90.0 : 40.0;
            FakeWorkload::step(now, rec);
        }
    } w;
    w.declared = {{SinkKind::None, "", ""}};
    auto res = r.run(*findScenario("BEN-01"), w, 1);
    REQUIRE(res.ok());
    CHECK(res.value().median.value("cpu.process.avg") == doctest::Approx(40.0));
    CHECK(res.value().worst.value("cpu.process.avg") == doctest::Approx(90.0));
    CHECK(res.value().gatesWorst.verdict == Verdict::Fail);
    CHECK(res.value().gatesMedian.verdict == Verdict::Pass);
}

TEST_CASE("REQ-P02: runner refuses live outputs, recording, lighting and broadcast sinks before preparing") {
    for (SinkKind k : {SinkKind::LiveOutput, SinkKind::LiveRecording, SinkKind::LiveLighting, SinkKind::Broadcast}) {
        FakeBenchTime time;
        BenchmarkRunner r(time, fastCfg());
        FakeWorkload w; w.declared = {{SinkKind::Mock, "ok", ""}, {k, "live", "udp://127.0.0.1:6454"}};
        auto res = r.run(*findScenario("BEN-06"), w, 1);
        INFO(sinkKindName(k));
        REQUIRE_FALSE(res.ok());
        CHECK(res.error().category == ErrorCategory::PermissionDenied);
        CHECK_FALSE(w.prepared);                         // nothing was started
        CHECK(w.steps == 0);
        CHECK(time.nowMonotonic() == 0);
    }
}

TEST_CASE("REQ-P02: discard receivers must be loopback; remote endpoints are refused") {
    CHECK(isLoopbackEndpoint(""));
    CHECK(isLoopbackEndpoint("127.0.0.1:1935"));
    CHECK(isLoopbackEndpoint("rtmp://localhost:1935/live"));
    CHECK(isLoopbackEndpoint("srt://[::1]:9000"));
    CHECK(isLoopbackEndpoint("127.8.9.10"));
    CHECK_FALSE(isLoopbackEndpoint("rtmp://a.rtmp.youtube.com/live2"));
    CHECK_FALSE(isLoopbackEndpoint("10.0.0.5:1935"));
    CHECK_FALSE(isLoopbackEndpoint("localhost.example.com"));
    CHECK_FALSE(isLoopbackEndpoint("127.evil.com"));
    CHECK_FALSE(isLoopbackEndpoint("rtmp://user:key@127.0.0.1/live"));   // credentials in the URL
    CHECK_FALSE(isLoopbackEndpoint("128.0.0.1"));
    CHECK_FALSE(isLoopbackEndpoint("127.0.0.256"));
    CHECK(sinkAllowed({SinkKind::LocalDiscard, "d", "127.0.0.1:9"}));
    CHECK_FALSE(sinkAllowed({SinkKind::LocalDiscard, "d", "203.0.113.9:9"}));
    CHECK_FALSE(sinkAllowed({SinkKind::Broadcast, "b", "127.0.0.1:9"}));    // broadcast kind is never allowed, even locally

    FakeBenchTime time;
    BenchmarkRunner r(time, fastCfg());
    FakeWorkload w; w.declared = {{SinkKind::LocalDiscard, "d", "203.0.113.9:9"}};
    CHECK_FALSE(r.run(*findScenario("BEN-05"), w, 1).ok());
    FakeWorkload ok; ok.declared = {{SinkKind::LocalDiscard, "d", "127.0.0.1:9"}};
    CHECK(r.run(*findScenario("BEN-05"), ok, 1).ok());
}

TEST_CASE("REQ-P02: a scenario only accepts the benign sinks it declares") {
    FakeBenchTime time;
    BenchmarkRunner r(time, fastCfg());
    FakeWorkload w; w.declared = {{SinkKind::DisposableFile, "tmp", ""}};
    CHECK_FALSE(r.run(*findScenario("BEN-01"), w, 1).ok());     // BEN-01 has no recording sink
    CHECK(r.run(*findScenario("BEN-04"), w, 1).ok());
}

TEST_CASE("REQ-P01: suite records skipped tests and continues past refusals") {
    FakeBenchTime time;
    BenchmarkRunner r(time, fastCfg());
    FakeWorkload ok1; ok1.declared = {{SinkKind::Mock, "", ""}};
    FakeWorkload bad; bad.declared = {{SinkKind::Broadcast, "", "rtmp://example.com/x"}};
    FakeWorkload ok2; ok2.declared = {{SinkKind::None, "", ""}};
    std::vector<SuiteEntry> entries = {{"BEN-01", &ok1, ""}, {"BEN-03", nullptr, "no camera attached"},
                                       {"BEN-05", &bad, ""}, {"BEN-02", &ok2, ""}, {"BEN-77", &ok2, ""}};
    auto res = r.runSuite(env(), entries, 7);
    REQUIRE(res.ok());
    CHECK(res.value().scenarios.size() == 2);
    REQUIRE(res.value().skipped.size() == 3);
    CHECK(res.value().skipped[0].scenarioId == "BEN-03");
    CHECK(res.value().skipped[0].reason == "no camera attached");
    CHECK(res.value().skipped[1].scenarioId == "BEN-05");
    CHECK(res.value().skipped[1].reason.find("refused") != std::string::npos);
    CHECK(res.value().skipped[2].reason == "unknown scenario");
}

TEST_CASE("T-FAULT: operator cancel stops the run and still finishes the workload") {
    FakeBenchTime time;
    std::atomic<bool> cancel{true};
    RunnerConfig cfg = fastCfg(); cfg.cancel = &cancel;
    BenchmarkRunner r(time, cfg);
    FakeWorkload w; w.declared = {{SinkKind::None, "", ""}};
    auto res = r.run(*findScenario("BEN-01"), w, 1);
    REQUIRE_FALSE(res.ok());
    CHECK(res.error().category == ErrorCategory::Cancelled);
    CHECK(w.finished);
}

TEST_CASE("REQ-P01: invalid runner configuration and workload failure are reported") {
    FakeBenchTime time;
    RunnerConfig cfg = fastCfg(); cfg.runs = 0;
    BenchmarkRunner bad(time, cfg);
    FakeWorkload w; w.declared = {{SinkKind::None, "", ""}};
    CHECK(bad.run(*findScenario("BEN-01"), w, 1).error().category == ErrorCategory::InvalidArgument);
    BenchmarkRunner r(time, fastCfg());
    FakeWorkload f; f.declared = {{SinkKind::None, "", ""}};
    f.prepareStatus = Status(makeError(ErrorCategory::UnsupportedCapability, "no camera"));
    auto res = r.run(*findScenario("BEN-03"), f, 1);
    REQUIRE_FALSE(res.ok());
    CHECK(res.error().category == ErrorCategory::UnsupportedCapability);
}

TEST_CASE("REQ-P01: report JSON is deterministic and records the required environment") {
    auto make = [] {
        FakeBenchTime time;
        BenchmarkRunner r(time, fastCfg());
        FakeWorkload w; w.declared = {{SinkKind::Mock, "", ""}};
        std::vector<SuiteEntry> e = {{"BEN-01", &w, ""}, {"BEN-04", nullptr, "no encoder"}};
        return r.runSuite(env(), e, 99).value();
    };
    const SuiteReport a = make(), b = make();
    const std::string ja = toJson(a), jb = toJson(b);
    CHECK(ja == jb);                                       // same fixture => comparable, byte-identical record
    auto j = nlohmann::json::parse(ja);
    CHECK(j["schema"] == "stagesignal.benchmark/1");
    const auto& e = j["environment"];
    CHECK(e["appHash"] == "abc123");
    CHECK(e["dependencyHashes"]["juce"] == "h1");
    CHECK(e["os"] == "TestOS 1");
    CHECK(e["powerPlan"] == "High performance");
    CHECK(e["cpu"] == "Test CPU 800 MHz");
    CHECK(e.contains("gpu")); CHECK(e.contains("gpuDriver"));
    CHECK(e["monitors"].size() == 2);
    CHECK(e["compositions"][0]["width"] == 800);
    CHECK(e["capture"]["rateHz"] == 48000);
    CHECK(e["mediaHashes"][0] == "m1");
    CHECK(e.contains("encoder"));
    CHECK(e["encoder"].contains("bitrateKbps"));
    const auto& s = j["scenarios"][0];
    CHECK(s["id"] == "BEN-01");
    CHECK(s["runs"].size() == 3);
    CHECK(s["median"]["fps.mean"].is_number());
    CHECK(s["worst"].contains("interval.p99.ms"));
    CHECK(s["warmup"]["frames"].is_number());
    CHECK(s["gatesWorst"]["gates"].size() > 10);
    CHECK(s["verdict"] == "pass");
    CHECK(j["skipped"][0]["scenario"] == "BEN-04");
    CHECK(j["skipped"][0]["reason"] == "no encoder");
    CHECK(j["profileHash"] == a.profileHash());
}

TEST_CASE("REQ-P01: profile hash depends on the environment, not on measurements") {
    SuiteReport a, b;
    a.env = env(); b.env = env();
    CHECK(a.profileHash() == b.profileHash());
    CHECK(a.profileHash().size() == 64);
    CHECK(environmentHash(a.env) == a.profileHash());
    b.env.powerPlan = "Balanced";
    CHECK(a.profileHash() != b.profileHash());
    b.env = env(); b.env.monitors[0].rotationDeg = 90;
    CHECK(a.profileHash() != b.profileHash());
    b.env = env(); b.env.appHash = "other";
    CHECK(a.profileHash() != b.profileHash());
}

TEST_CASE("REQ-P01: unavailable metrics serialise as null, never zero") {
    FakeBenchTime time;
    BenchmarkRunner r(time, fastCfg());
    RunShape s; s.cpuAvailable = false;
    FakeWorkload w(s); w.declared = {{SinkKind::None, "", ""}};
    auto res = r.run(*findScenario("BEN-01"), w, 1);
    REQUIRE(res.ok());
    SuiteReport rep; rep.scenarios.push_back(res.value());
    auto j = nlohmann::json::parse(toJson(rep));
    CHECK(j["scenarios"][0]["median"]["cpu.process.avg"].is_null());
    CHECK(res.value().gatesWorst.verdict == Verdict::Incomplete);
}
