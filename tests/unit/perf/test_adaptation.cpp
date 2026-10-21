#include "doctest/doctest.h"
#include <vector>
#include "ss/perf/adaptation.hpp"

using namespace ss;
using namespace ss::perf;

namespace {
struct Load { double cpu = 30; double miss = 0.0; double queue = 0.0; bool queueAvail = true; };

AdaptationEvent tick(AdaptationController& c, Ns t, const Load& l) {
    PressureSample s;
    s.now = t; s.cpuAvailable = true; s.cpuPct = l.cpu;
    s.deadlinesDue = 100; s.deadlinesMissed = static_cast<std::uint64_t>(l.miss * 100.0 + 0.5);
    s.queueAvailable = l.queueAvail; s.encoderQueue = l.queue;
    return c.update(s);
}

struct Change { Ns t; AdaptationEvent ev; };
// Runs 1 s ticks from `from` (inclusive) to `to` (exclusive, seconds) under a constant load.
std::vector<Change> drive(AdaptationController& c, int from, int to, const Load& l) {
    std::vector<Change> out;
    for (int s = from; s < to; ++s) {
        AdaptationEvent e = tick(c, s * kNsPerSec, l);
        if (e.kind != AdaptationEvent::Kind::None) out.push_back({s * kNsPerSec, e});
    }
    return out;
}
}  // namespace

TEST_CASE("REQ-P03: sustained CPU load produces the section 7 sequence one step per 5 s") {
    AdaptationController c;                 // initialSceneFps = 30 => 4 + 3 steps
    CHECK(c.maxStep() == 7);
    auto all = drive(c, 0, 60, Load{95, 0, 0});
    std::vector<Change> ev;
    for (const Change& x : all) if (x.ev.kind == AdaptationEvent::Kind::Shed) ev.push_back(x);
    REQUIRE(ev.size() == 7);
    // the persistent overload at the last step is surfaced as exactly one warning, after the last shed
    REQUIRE(all.size() == 8);
    CHECK(all.back().ev.kind == AdaptationEvent::Kind::OverloadWarning);
    const AdaptStepKind expect[] = {AdaptStepKind::PreviewFps, AdaptStepKind::ReduceEffects, AdaptStepKind::LowerRenderScale,
                                    AdaptStepKind::ReduceSourceRequest, AdaptStepKind::LowerSceneFps,
                                    AdaptStepKind::LowerSceneFps, AdaptStepKind::LowerSceneFps};
    for (std::size_t i = 0; i < ev.size(); ++i) {
        CHECK(ev[i].ev.kind == AdaptationEvent::Kind::Shed);
        CHECK(ev[i].ev.stepKind == expect[i]);
        CHECK(ev[i].ev.toStep == static_cast<int>(i) + 1);
        CHECK(ev[i].t == static_cast<Ns>(5 + 5 * i) * kNsPerSec);   // first at 5 s, then exactly 5 s apart
    }
    AdaptationState s = c.state();
    CHECK(s.previewFps == doctest::Approx(5.0));
    CHECK(s.effectsScale < 1.0);
    CHECK(s.renderScale < 1.0);
    CHECK(s.sourceRequestReduced);
    CHECK(s.sceneFps == 15);
    CHECK(s.sceneFpsLowered);
    // the three scene FPS steps visit 25, 20, 15 in that order
    AdaptationController c2;
    drive(c2, 0, 25, Load{95, 0, 0});
    CHECK(c2.step() == 4); CHECK(c2.state().sceneFps == 30);
    drive(c2, 25, 30, Load{95, 0, 0}); CHECK(c2.state().sceneFps == 25);
    drive(c2, 30, 35, Load{95, 0, 0}); CHECK(c2.state().sceneFps == 20);
    drive(c2, 35, 40, Load{95, 0, 0}); CHECK(c2.state().sceneFps == 15);
    // no step beyond the last and no repeated warning
    CHECK(drive(c, 60, 90, Load{95, 0, 0}).empty());
    CHECK(c.step() == 7);
}

TEST_CASE("REQ-P03: each trigger fires on its own threshold and duration") {
    {   // CPU: 85% for 5 s, but not 84.9%
        AdaptationController c;
        CHECK(drive(c, 0, 30, Load{84.9, 0, 0}).empty());
        AdaptationController d;
        auto ev = drive(d, 0, 6, Load{85.0, 0, 0});
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].t == 5 * kNsPerSec);
    }
    {   // CPU for only 4 s then relief: no step
        AdaptationController c;
        CHECK(drive(c, 0, 4, Load{95, 0, 0}).empty());
        CHECK(drive(c, 4, 20, Load{50, 0, 0}).empty());
    }
    {   // Missed deadlines: >5% over 5 s. Exactly 5% does not trigger.
        AdaptationController c;
        CHECK(drive(c, 0, 30, Load{30, 0.05, 0}).empty());
        AdaptationController d;
        auto ev = drive(d, 0, 8, Load{30, 0.08, 0});
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].t == 5 * kNsPerSec);
    }
    {   // Encoder queue >= 75% for 2 s
        AdaptationController c;
        auto ev = drive(c, 0, 4, Load{30, 0, 0.75});
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].t == 2 * kNsPerSec);
        AdaptationController d;
        CHECK(drive(d, 0, 30, Load{30, 0, 0.74}).empty());
        AdaptationController e;
        CHECK(drive(e, 0, 1, Load{30, 0, 0.9}).empty());       // 1 s is not enough
    }
}

TEST_CASE("REQ-P03: restore is one step per 30 s after CPU, misses and queues stay calm for 30 s") {
    AdaptationController c;
    auto shed = drive(c, 0, 20, Load{95, 0, 0});       // steps at 5, 10, 15
    REQUIRE(shed.size() == 3);
    // Moderately loaded (between 65 and 85): hysteresis band, nothing happens in either direction.
    CHECK(drive(c, 20, 80, Load{75, 0, 0}).empty());
    CHECK(c.step() == 3);
    // Calm from t=80: needs 30 s calm => first restore at t=110, then every 30 s.
    auto up = drive(c, 80, 200, Load{50, 0, 0.1});
    REQUIRE(up.size() == 3);
    for (const auto& u : up) CHECK(u.ev.kind == AdaptationEvent::Kind::Restore);
    CHECK(up[0].t == 110 * kNsPerSec);                 // calm since t=80, 30 s later
    CHECK(up[1].t - up[0].t == 30 * kNsPerSec);
    CHECK(up[2].t - up[1].t == 30 * kNsPerSec);
    CHECK(c.step() == 0);
    CHECK(c.state().previewFps == doctest::Approx(c.config().normalPreviewFps));
}

TEST_CASE("REQ-P03: calm conditions are all required (CPU, misses, queue)") {
    for (int which = 0; which < 3; ++which) {
        AdaptationController c;
        drive(c, 0, 6, Load{95, 0, 0});
        REQUIRE(c.step() == 1);
        Load l{50, 0, 0.1};
        if (which == 0) l.cpu = 66;           // CPU not below 65
        if (which == 1) l.miss = 0.02;        // misses not below 1% (but below the 5% trigger)
        if (which == 2) l.queue = 0.30;       // queue not below 25%
        auto ev = drive(c, 6, 200, l);
        INFO("blocker " << which);
        CHECK(ev.empty());
        CHECK(c.step() == 1);
    }
}

TEST_CASE("REQ-P03: hysteresis prevents flapping around the thresholds") {
    AdaptationController c;
    int changes = 0;
    for (int s = 0; s < 600; ++s) {
        const double cpu = (s / 3) % 2 ? 90.0 : 60.0;      // 3 s high, 3 s low: never 5 s sustained, never 30 s calm
        if (tick(c, s * kNsPerSec, Load{cpu, 0, 0}).kind != AdaptationEvent::Kind::None) ++changes;
    }
    CHECK(changes == 0);
    // After a shed, a brief dip in CPU does not restore.
    AdaptationController d;
    drive(d, 0, 6, Load{95, 0, 0});
    CHECK(d.step() == 1);
    CHECK(drive(d, 6, 20, Load{50, 0, 0}).empty());
    CHECK(d.step() == 1);
}

TEST_CASE("REQ-P04: locked quality is never changed, warns once and rejects added work") {
    AdaptationController c;
    c.setLocked(true);
    auto ev = drive(c, 0, 60, Load{99, 0, 0});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].ev.kind == AdaptationEvent::Kind::OverloadWarning);
    CHECK(c.step() == 0);
    CHECK(c.state().sceneFps == 30);
    CHECK(c.overloadWarningActive());
    std::string why;
    CHECK_FALSE(c.admitAddedWork(&why));
    CHECK_FALSE(why.empty());
    // After recovery the warning clears and work is admitted again; quality was never touched.
    drive(c, 60, 120, Load{40, 0, 0});
    CHECK_FALSE(c.overloadWarningActive());
    CHECK(c.admitAddedWork());
    CHECK(c.step() == 0);
}

TEST_CASE("REQ-P04: locking while shed freezes the current level (no further shed, no restore)") {
    AdaptationController c;
    drive(c, 0, 11, Load{95, 0, 0});
    REQUIRE(c.step() == 2);
    c.setLocked(true);
    drive(c, 11, 200, Load{95, 0, 0});
    CHECK(c.step() == 2);
    drive(c, 200, 400, Load{30, 0, 0});
    CHECK(c.step() == 2);
}

TEST_CASE("REQ-P03: unavailable CPU counter never triggers or restores; unavailable queue does not block restore") {
    AdaptationController c;
    PressureSample s;
    s.deadlinesDue = 100;
    for (int i = 0; i < 100; ++i) { s.now = i * kNsPerSec; CHECK(c.update(s).kind == AdaptationEvent::Kind::None); }
    AdaptationController d;
    drive(d, 0, 6, Load{95, 0, 0});
    REQUIRE(d.step() == 1);
    for (int i = 6; i < 200; ++i) { PressureSample u; u.now = i * kNsPerSec; u.deadlinesDue = 100; d.update(u); }
    CHECK(d.step() == 1);                                    // unknown CPU is not proof of calm
    auto ev = drive(d, 200, 300, Load{30, 0, 0, false});     // CPU known, no encoder queue present
    CHECK(ev.size() == 1);
    CHECK(d.step() == 0);
}

TEST_CASE("REQ-P03: BASE profile starts at 25 FPS so the scene-FPS ladder is 25-to-20-to-15 only") {
    AdaptationConfig cfg; cfg.initialSceneFps = 25;
    AdaptationController c(cfg);
    CHECK(c.maxStep() == 6);
    drive(c, 0, 200, Load{99, 0, 0});
    CHECK(c.state().sceneFps == 15);
    AdaptationController d(cfg);
    drive(d, 0, 26, Load{99, 0, 0});                         // 5 steps: preview, effects, scale, source, 25->20
    CHECK(d.state().sceneFps == 20);
    CHECK(d.state().sceneFpsLowered);
}

TEST_CASE("REQ-P03: tunable config is validated and invalid values are rejected without side effects") {
    AdaptationConfig bad;
    bad.cpuTriggerPct = 40; CHECK_FALSE(bad.validate().ok());
    bad = {}; bad.cpuCalmPct = 90; CHECK_FALSE(bad.validate().ok());
    bad = {}; bad.shedIntervalNs = 0; CHECK_FALSE(bad.validate().ok());
    bad = {}; bad.queueCalm = 0.8; CHECK_FALSE(bad.validate().ok());
    bad = {}; bad.missCalmFraction = 0.06; CHECK_FALSE(bad.validate().ok());
    bad = {}; bad.initialSceneFps = 10; CHECK_FALSE(bad.validate().ok());
    CHECK(bad.validate().error().category == ErrorCategory::InvalidArgument);
    CHECK(AdaptationConfig{}.validate().ok());

    AdaptationController c;
    CHECK_FALSE(c.setConfig(bad).ok());
    CHECK(c.config().initialSceneFps == 30);

    AdaptationConfig fast; fast.cpuTriggerNs = 2 * kNsPerSec; fast.shedIntervalNs = 2 * kNsPerSec;
    REQUIRE(c.setConfig(fast).ok());
    auto ev = drive(c, 0, 5, Load{95, 0, 0});
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].t == 2 * kNsPerSec);
    CHECK(ev[1].t == 4 * kNsPerSec);
}

TEST_CASE("T-FAULT: encoder queue stuck full sheds, then restores once the sink recovers") {
    AdaptationController c;
    auto shed = drive(c, 0, 12, Load{40, 0, 0.9});
    CHECK(shed.size() >= 2);
    const int stepAtFault = c.step();
    auto rec = drive(c, 12, 400, Load{40, 0, 0.05});
    CHECK(c.step() == 0);
    CHECK(static_cast<int>(rec.size()) == stepAtFault);
}

TEST_CASE("REQ-P03: added work is refused while still shedding") {
    AdaptationController c;
    CHECK(c.admitAddedWork());
    drive(c, 0, 6, Load{95, 0, 0});
    std::string why;
    CHECK_FALSE(c.admitAddedWork(&why));
    drive(c, 6, 400, Load{30, 0, 0});
    CHECK(c.admitAddedWork());
}
