#include "doctest/doctest.h"
#include "perf_fixtures.hpp"
#include "ss/perf/admission.hpp"

using namespace perftest;

namespace {
constexpr std::int64_t kDay = 24LL * 3600 * 1000;

SuiteReport reportWith(double cpu, std::vector<std::pair<std::string, Verdict>> verdicts, std::int64_t at) {
    SuiteReport r;
    r.env.appHash = "app1"; r.env.os = "TestOS";
    r.capturedAtUnixMs = at;
    for (auto& v : verdicts) {
        ScenarioReport s; s.scenarioId = v.first;
        RunShape sh; sh.cpuPct = cpu;
        s.median = synthRun(sh);
        s.gatesWorst.verdict = v.second;
        r.scenarios.push_back(s);
    }
    return r;
}
FeatureDef camera() { FeatureDef f; f.id = "camera"; f.requiredScenario = "BEN-03"; f.cpuCostPct = 10; f.memoryBytes = 8 * kMiB; f.requiredDevices = {"camera0"}; return f; }
FeatureDef avRecord() { FeatureDef f; f.id = "av-record"; f.minTier = Tier::Standard; f.requiredScenario = "BEN-04"; f.cpuCostPct = 20; f.memoryBytes = 6 * kMiB; f.needsEncoder = true; return f; }
FeatureDef secondOutput() { FeatureDef f; f.id = "second-output"; f.requiredScenario = "BEN-02"; f.cpuCostPct = 8; f.memoryBytes = 4 * kMiB; return f; }

AdmissionInput baseInput(const BenchmarkProfile* p, const std::string& hash, std::int64_t now) {
    AdmissionInput in;
    in.tier = Tier::Standard; in.currentProfileHash = hash; in.nowUnixMs = now; in.profile = p;
    in.availableDevices = {"camera0"}; in.encoderAvailable = true;
    return in;
}
const FeatureDecision& dec(const AdmissionResult& r, const std::string& id) {
    for (const auto& d : r.decisions) if (d.id == id) return d;
    static FeatureDecision none; return none;
}
}  // namespace

TEST_CASE("REQ-P01: tier classification follows section 6 minimums") {
    HardwareInfo hw;
    hw.logicalCores = 1; hw.installedRamBytes = 512 * kMiB; hw.displays = 2;
    auto c = classifyTier(hw);
    CHECK(c.tier == Tier::Base); CHECK(c.meetsBaseFloor);
    hw.installedRamBytes = 256 * kMiB;
    CHECK_FALSE(classifyTier(hw).meetsBaseFloor);
    hw.installedRamBytes = 2048 * kMiB; hw.logicalCores = 2;
    CHECK(classifyTier(hw).tier == Tier::Standard);
    hw.logicalCores = 4; hw.installedRamBytes = 8192 * kMiB; hw.displays = 3;
    CHECK(classifyTier(hw).tier == Tier::Standard);         // no SSD/GPU => not ENHANCED
    hw.ssd = true; hw.gpuSuitable = true;
    CHECK(classifyTier(hw).tier == Tier::Enhanced);
    hw.installedRamBytes = 16384 * kMiB;
    CHECK(classifyTier(hw).tier == Tier::Enhanced);         // SHOW also needs a qualified encoder
    hw.qualifiedEncoder = true;
    CHECK(classifyTier(hw).tier == Tier::Show);
    CHECK(std::string(tierName(Tier::Base)) == "BASE");
    CHECK(tierDef(Tier::Base).targetFps == 25);
    CHECK(tierDef(Tier::Base).compositionW == 800);
}

TEST_CASE("REQ-P06: BASE memory sub-budgets are UI 32, frames 12, DSP 8, source cache 8, transport 4 MiB") {
    MemoryBudget b = memoryBudget(Tier::Base);
    CHECK(b.sub[(int)MemCategory::Ui] == 32 * kMiB);
    CHECK(b.sub[(int)MemCategory::Frames] == 12 * kMiB);
    CHECK(b.sub[(int)MemCategory::Dsp] == 8 * kMiB);
    CHECK(b.sub[(int)MemCategory::SourceCache] == 8 * kMiB);
    CHECK(b.sub[(int)MemCategory::Transport] == 4 * kMiB);
    CHECK(b.steadyCap == 96 * kMiB); CHECK(b.peakCap == 128 * kMiB); CHECK(b.hostCap == 64 * kMiB);
    CHECK_FALSE(b.provisional);
    CHECK(memoryBudget(Tier::Standard).provisional);
    CHECK(memoryBudget(Tier::Standard).steadyCap == 4 * 96 * kMiB);
}

TEST_CASE("REQ-P06: memory accountant enforces sub-budgets, aggregate cap and transient peak") {
    MemoryAccountant a(memoryBudget(Tier::Base));
    CHECK(a.reserve(MemCategory::Frames, 12 * kMiB).ok());
    CHECK_FALSE(a.reserve(MemCategory::Frames, 1).ok());                   // sub-budget full
    CHECK(a.reserve(MemCategory::Frames, 1).error().category == ErrorCategory::ResourceExhausted);
    CHECK(a.reserve(MemCategory::Ui, 32 * kMiB).ok());
    CHECK(a.reserve(MemCategory::Dsp, 8 * kMiB).ok());
    CHECK(a.reserve(MemCategory::SourceCache, 8 * kMiB).ok());
    CHECK(a.reserve(MemCategory::Transport, 4 * kMiB).ok());
    CHECK(a.reserve(MemCategory::Runtime, 32 * kMiB).ok());
    CHECK(a.total() == 96 * kMiB);
    CHECK(a.remaining(MemCategory::Runtime) == 0);
    // transient growth may reach the 128 MiB peak cap but not beyond
    CHECK(a.reserveTransient(MemCategory::SourceCache, 8 * kMiB).ok());
    CHECK(a.reserveTransient(MemCategory::Frames, 12 * kMiB).ok());
    CHECK(a.reserveTransient(MemCategory::Ui, 12 * kMiB).ok());           // 96 + 8 + 12 + 12 = 128
    CHECK_FALSE(a.reserveTransient(MemCategory::Ui, 1).ok());
    CHECK(a.peak() == 128 * kMiB);
    a.release(MemCategory::Ui, 100 * kMiB);                               // over-release clamps at zero
    CHECK(a.used(MemCategory::Ui) == 0);
    CHECK(a.reserve(MemCategory::Ui, 1).ok());
}

TEST_CASE("REQ-P06: aggregate cap blocks a reserve even when the sub-budget has room") {
    MemoryBudget b = memoryBudget(Tier::Base);
    b.steadyCap = 40 * kMiB;
    MemoryAccountant a(b);
    CHECK(a.reserve(MemCategory::Ui, 32 * kMiB).ok());
    CHECK_FALSE(a.reserve(MemCategory::Frames, 9 * kMiB).ok());
    CHECK(a.reserve(MemCategory::Frames, 8 * kMiB).ok());
}

TEST_CASE("REQ-P01: admission admits with a matching, recent, passing profile and reports IF-09 status") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(30, {{"BEN-01", Verdict::Pass}, {"BEN-02", Verdict::Pass}, {"BEN-03", Verdict::Pass}}, now - 2 * kDay);
    BenchmarkProfile p = profileFromReport(rep);
    AdmissionInput in = baseInput(&p, rep.profileHash(), now);
    in.requested = {camera(), secondOutput()};
    AdmissionResult r = admitFeatures(in);
    CHECK(dec(r, "camera").state == AdmissionState::Admitted);
    CHECK(dec(r, "second-output").state == AdmissionState::Admitted);
    CHECK(r.baselineKnown);
    CHECK(r.cpuHeadroomPct == doctest::Approx(65 - 5 - 30));
    CHECK(r.status.metricsAvailable);
    CHECK(r.status.profileHash == rep.profileHash());
    CHECK(r.status.admitted.size() == 2);
    CHECK(r.status.rejected.empty());
    CHECK(r.status.cpuPct == doctest::Approx(30));
    CHECK(r.status.fps == doctest::Approx(25.0).epsilon(0.02));
}

TEST_CASE("REQ-P01: no, stale, foreign or future benchmark gives the 'untested, run benchmark' state") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(30, {{"BEN-01", Verdict::Pass}, {"BEN-03", Verdict::Pass}}, now - kDay);
    BenchmarkProfile p = profileFromReport(rep);
    {   AdmissionInput in = baseInput(nullptr, rep.profileHash(), now); in.requested = {camera()};
        auto r = admitFeatures(in);
        CHECK(dec(r, "camera").state == AdmissionState::Untested);
        CHECK(dec(r, "camera").reasons[0].find("untested, run benchmark") == 0);
        CHECK_FALSE(r.status.metricsAvailable);
        CHECK(r.status.rejected.size() == 1); }
    {   AdmissionInput in = baseInput(&p, "different-hash", now); in.requested = {camera()};
        CHECK(dec(admitFeatures(in), "camera").state == AdmissionState::Untested); }
    {   AdmissionInput in = baseInput(&p, rep.profileHash(), now + 60 * kDay); in.requested = {camera()};
        auto r = admitFeatures(in);
        CHECK(dec(r, "camera").state == AdmissionState::Untested);
        CHECK(dec(r, "camera").reasons[0].find("too old") != std::string::npos); }
    {   AdmissionInput in = baseInput(&p, rep.profileHash(), now - 10 * kDay); in.requested = {camera()};
        CHECK(dec(admitFeatures(in), "camera").state == AdmissionState::Untested); }       // timestamp in the future
    {   AdmissionInput in = baseInput(&p, rep.profileHash(), now); in.requested = {avRecord()};   // BEN-04 never ran
        auto r = admitFeatures(in);
        CHECK(dec(r, "av-record").state == AdmissionState::Untested); }
}

TEST_CASE("REQ-P01: failed benchmark, missing device, encoder or tier reject with reasons") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(30, {{"BEN-01", Verdict::Pass}, {"BEN-03", Verdict::Fail}, {"BEN-04", Verdict::Pass}}, now);
    BenchmarkProfile p = profileFromReport(rep);
    AdmissionInput in = baseInput(&p, rep.profileHash(), now);
    in.requested = {camera(), avRecord()};
    auto r = admitFeatures(in);
    CHECK(dec(r, "camera").state == AdmissionState::Rejected);
    CHECK(dec(r, "camera").reasons[0].find("did not pass") != std::string::npos);
    CHECK(dec(r, "av-record").state == AdmissionState::Admitted);

    in.availableDevices.clear(); in.encoderAvailable = false;
    in.requested = {camera(), avRecord()};
    r = admitFeatures(in);
    CHECK(dec(r, "camera").state == AdmissionState::Rejected);
    CHECK(dec(r, "av-record").state == AdmissionState::Rejected);
    CHECK(dec(r, "av-record").reasons[0] == "no encoder available");

    in.tier = Tier::Base; in.encoderAvailable = true; in.requested = {avRecord()};
    r = admitFeatures(in);
    CHECK(dec(r, "av-record").state == AdmissionState::Rejected);
    CHECK(dec(r, "av-record").reasons[0].find("STANDARD") != std::string::npos);
    CHECK_FALSE(dec(r, "av-record").overridable);
}

TEST_CASE("REQ-P01: cumulative CPU and memory headroom is consumed in request order") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(40, {{"BEN-01", Verdict::Pass}, {"BEN-02", Verdict::Pass}, {"BEN-03", Verdict::Pass}, {"BEN-04", Verdict::Pass}}, now);
    BenchmarkProfile p = profileFromReport(rep);
    AdmissionInput in = baseInput(&p, rep.profileHash(), now);
    // headroom = 65 - 5 - 40 = 20: camera (10) fits, second-output (8) fits, av-record (20) does not
    in.requested = {camera(), secondOutput(), avRecord()};
    auto r = admitFeatures(in);
    CHECK(dec(r, "camera").state == AdmissionState::Admitted);
    CHECK(dec(r, "second-output").state == AdmissionState::Admitted);
    CHECK(dec(r, "av-record").state == AdmissionState::Rejected);
    CHECK(dec(r, "av-record").reasons[0].find("headroom") != std::string::npos);
    CHECK(dec(r, "av-record").overridable);
    // different order, different outcome: deterministic and order-dependent by design
    in.requested = {avRecord(), camera(), secondOutput()};
    r = admitFeatures(in);
    CHECK(dec(r, "av-record").state == AdmissionState::Admitted);
    CHECK(dec(r, "camera").state == AdmissionState::Rejected);
    // memory budget
    FeatureDef big = secondOutput(); big.id = "big"; big.cpuCostPct = 1; big.memoryBytes = 200 * kMiB;
    in.requested = {big};
    CHECK(dec(admitFeatures(in), "big").reasons[0].find("MiB") != std::string::npos);
}

TEST_CASE("REQ-P01: documented override lifts soft rejections only and needs a reason") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(50, {{"BEN-01", Verdict::Pass}, {"BEN-04", Verdict::Pass}}, now);
    BenchmarkProfile p = profileFromReport(rep);
    AdmissionInput in = baseInput(&p, rep.profileHash(), now);
    in.requested = {avRecord(), camera()};
    in.overrides = {{"av-record", ""}, {"camera", "venue camera tested by hand"}};
    auto r = admitFeatures(in);
    CHECK(dec(r, "av-record").state == AdmissionState::Rejected);          // empty reason: ignored
    CHECK(dec(r, "av-record").reasons.back().find("reason is required") != std::string::npos);
    CHECK(dec(r, "camera").state == AdmissionState::AdmittedByOverride);   // BEN-03 never ran
    CHECK(dec(r, "camera").reasons.back().find("venue camera tested by hand") != std::string::npos);
    CHECK(r.status.admitted.size() == 1);

    in.availableDevices.clear();                                           // hard failure: no override possible
    r = admitFeatures(in);
    CHECK(dec(r, "camera").state == AdmissionState::Rejected);
    CHECK_FALSE(dec(r, "camera").overridable);
}

TEST_CASE("REQ-P04: locked and overloaded quality rejects added work without override") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(20, {{"BEN-01", Verdict::Pass}, {"BEN-02", Verdict::Pass}}, now);
    BenchmarkProfile p = profileFromReport(rep);
    AdmissionInput in = baseInput(&p, rep.profileHash(), now);
    in.requested = {secondOutput()};
    in.overrides = {{"second-output", "just do it"}};
    in.qualityLocked = true; in.overloaded = true; in.adaptationStep = 0;
    auto r = admitFeatures(in);
    CHECK(dec(r, "second-output").state == AdmissionState::Rejected);
    CHECK(dec(r, "second-output").reasons[0].find("locked") != std::string::npos);
    in.overloaded = false;
    CHECK(dec(admitFeatures(in), "second-output").state == AdmissionState::Admitted);
    in.adaptationStep = 3;
    CHECK(admitFeatures(in).status.adaptationStep == 3);
}

TEST_CASE("REQ-P01: admission output is deterministic") {
    const std::int64_t now = 100 * kDay;
    SuiteReport rep = reportWith(40, {{"BEN-01", Verdict::Pass}}, now);
    BenchmarkProfile p = profileFromReport(rep);
    AdmissionInput in = baseInput(&p, rep.profileHash(), now);
    in.requested = {camera(), secondOutput(), avRecord()};
    auto a = admitFeatures(in), b = admitFeatures(in);
    CHECK(a.status.reasons == b.status.reasons);
    CHECK(a.status.admitted == b.status.admitted);
    CHECK(a.status.rejected == b.status.rejected);
}

TEST_CASE("REQ-X06: soak passes a clean 4 h run") {
    SoakMonitor m;
    for (int i = 0; i <= 240; ++i) {
        SoakSample s; s.t = i * 60 * kNsPerSec;
        s.workingSetBytes = 70 * kMiB + (i % 5) * 100000; s.privateCommitBytes = 72 * kMiB;
        s.recordingFramesLost = 4; s.recordingLossDeclared = 4;
        m.add(s);
    }
    auto r = m.evaluate();
    for (const auto& c : r.checks) { INFO(c.id); CHECK(c.state == GateState::Pass); }
    CHECK(r.verdict == Verdict::Pass);
}

TEST_CASE("REQ-X06: soak fails on growth, crash, stuck blackout, undeclared loss and capture overruns") {
    auto build = [](auto mutate, int minutes = 240) {
        SoakMonitor m;
        for (int i = 0; i <= minutes; ++i) {
            SoakSample s; s.t = i * 60 * kNsPerSec; s.workingSetBytes = 70 * kMiB; s.privateCommitBytes = 72 * kMiB;
            mutate(m, s, i);
            m.add(s);
        }
        return m.evaluate();
    };
    auto find = [](const SoakReport& r, const char* id) { for (const auto& c : r.checks) if (c.id == id) return c.state; return GateState::Unavailable; };
    auto leak = build([](SoakMonitor&, SoakSample& s, int i) { s.workingSetBytes += static_cast<std::uint64_t>(i) * 200000; });   // ~48 MB over 4 h
    CHECK(find(leak, "S-WS-GROWTH") == GateState::Fail);
    CHECK(leak.verdict == Verdict::Fail);
    auto peak = build([](SoakMonitor&, SoakSample& s, int i) { if (i == 100) s.privateCommitBytes = 130 * kMiB; });
    CHECK(find(peak, "S-PEAK") == GateState::Fail);
    auto crash = build([](SoakMonitor& m, SoakSample&, int i) { if (i == 5) m.noteCrash(); });
    CHECK(find(crash, "S-CRASH") == GateState::Fail);
    auto stuck = build([](SoakMonitor&, SoakSample& s, int i) { if (i >= 50 && i < 60) { s.blackoutActive = true; s.blackoutRequested = false; } });
    CHECK(find(stuck, "S-BLACKOUT") == GateState::Fail);
    auto latched = build([](SoakMonitor&, SoakSample& s, int i) { if (i >= 50 && i < 60) { s.blackoutActive = true; s.blackoutRequested = true; } });
    CHECK(find(latched, "S-BLACKOUT") == GateState::Pass);          // operator-requested latch is not "stuck"
    auto loss = build([](SoakMonitor&, SoakSample& s, int i) { if (i >= 30) { s.recordingFramesLost = 10; s.recordingLossDeclared = 7; } });
    CHECK(find(loss, "S-REC-LOSS") == GateState::Fail);
    auto over = build([](SoakMonitor&, SoakSample& s, int i) { if (i >= 30) s.captureOverruns = 1; });
    CHECK(find(over, "S-CAPTURE-OVERRUNS") == GateState::Fail);
}

TEST_CASE("REQ-X06: a soak shorter than 4 h is incomplete, never a pass") {
    SoakMonitor m;
    for (int i = 0; i <= 60; ++i) { SoakSample s; s.t = i * 60 * kNsPerSec; s.workingSetBytes = 70 * kMiB; s.privateCommitBytes = 70 * kMiB; m.add(s); }
    CHECK(m.evaluate().verdict == Verdict::Incomplete);
    SoakMonitor empty;
    CHECK(empty.evaluate().verdict == Verdict::Incomplete);
    SoakConfig c; c.maxSamples = 10;
    SoakMonitor capped(c);
    for (int i = 0; i < 50; ++i) { SoakSample s; s.t = i; capped.add(s); }
    CHECK(capped.samples() == 10);
    CHECK(capped.evaluate().truncated);
}

TEST_CASE("REQ-P01: benchmark profile round-trips through bounded JSON and rejects bad input") {
    SuiteReport rep = reportWith(30, {{"BEN-01", Verdict::Pass}, {"BEN-03", Verdict::Fail}}, 12345);
    BenchmarkProfile p = profileFromReport(rep);
    auto back = profileFromJson(profileToJson(p));
    REQUIRE(back.ok());
    CHECK(back.value().profileHash == p.profileHash);
    CHECK(back.value().capturedAtUnixMs == 12345);
    REQUIRE(back.value().scenarios.size() == 2);
    CHECK(back.value().scenarios[1].verdict == Verdict::Fail);
    CHECK(back.value().scenarios[0].cpuPct == doctest::Approx(30.0));
    CHECK(profileToJson(back.value()) == profileToJson(p));
    CHECK_FALSE(profileFromJson("").ok());
    CHECK_FALSE(profileFromJson("not json").ok());
    CHECK_FALSE(profileFromJson("[1,2]").ok());
    CHECK_FALSE(profileFromJson("{\"schema\":\"other\"}").ok());
    CHECK_FALSE(profileFromJson("{\"schema\":\"stagesignal.profile/1\",\"scenarios\":{}}").ok());
    CHECK_FALSE(profileFromJson("{\"schema\":\"stagesignal.profile/1\",\"capturedAtUnixMs\":\"x\"}").ok());
    CHECK_FALSE(profileFromJson(std::string(70 * 1024, ' ')).ok());
}
