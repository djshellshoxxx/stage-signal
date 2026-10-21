// Energy Arc (DEC-13): build / drop detection, hysteresis, determinism, tunables.
#include "doctest/doctest.h"
#include "test_util.hpp"
#include "ss/audio/analyzer.hpp"
#include "ss/audio/energy_arc.hpp"

using namespace ss;
using namespace ss::audio;

namespace {
constexpr double kFps = 20.0;     // one push per control tick in the unit tests

struct Rec { std::vector<float> arc; std::vector<int> phase; };

struct Driver {
    EnergyArc a;
    Rec rec;
    double t = 0;
    Driver() { REQUIRE(a.prepare(kFps)); }
    void level(double db, double sec) {
        const int n = static_cast<int>(sec * kFps + 0.5);
        for (int i = 0; i < n; ++i) { a.push(std::pow(10.0, db / 10.0), false); rec.arc.push_back(a.arc()); rec.phase.push_back(static_cast<int>(a.phase())); t += 1.0 / kFps; }
    }
    void ramp(double db0, double db1, double sec) {
        const int n = static_cast<int>(sec * kFps + 0.5);
        for (int i = 0; i < n; ++i) { level(db0 + (db1 - db0) * (i + 1) / n, 1.0 / kFps); }
    }
    int phaseNow() const { return rec.phase.back(); }
};
bool anyPhase(const Rec& r, int ph, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to && i < r.phase.size(); ++i) if (r.phase[i] == ph) return true;
    return false;
}
}

TEST_CASE("Energy Arc: steady level never builds or drops, arc converges to the mapped level") {
    Driver d;
    d.level(-30.0, 60.0);
    CHECK_FALSE(anyPhase(d.rec, 2, 0, d.rec.phase.size()));
    // after the initial fade-in the phase is Steady
    CHECK_FALSE(anyPhase(d.rec, 1, static_cast<std::size_t>(20 * kFps), d.rec.phase.size()));
    CHECK(d.rec.arc.back() == doctest::Approx(0.4f).epsilon(0.02));
    for (float v : d.rec.arc) { CHECK(v >= 0.f); CHECK(v <= 1.f); }
}

TEST_CASE("Energy Arc: build = sustained rise, ends on a plateau; drop = sudden return after a dip") {
    Driver d;
    d.level(-35.0, 25.0);                                          // steady verse
    const std::size_t rampStart = d.rec.phase.size();
    CHECK_FALSE(anyPhase(d.rec, 1, static_cast<std::size_t>(15 * kFps), rampStart));
    d.ramp(-35.0, -12.0, 15.0);                                    // riser
    const std::size_t rampEnd = d.rec.phase.size();
    CHECK(anyPhase(d.rec, 1, rampStart, rampEnd));                 // Build seen during the ramp
    CHECK_FALSE(anyPhase(d.rec, 2, rampStart, rampEnd));
    // Build starts early enough to be useful (within 8 s of the ramp start)
    std::size_t firstBuild = rampStart;
    while (d.rec.phase[firstBuild] != 1) ++firstBuild;
    CHECK(double(firstBuild - rampStart) / kFps <= 8.0);
    d.level(-12.0, 12.0);                                          // plateau: Build must end (hysteresis)
    CHECK(d.phaseNow() == 0);
    CHECK_FALSE(anyPhase(d.rec, 2, rampEnd, d.rec.phase.size()));
    // breakdown dip then the drop
    d.level(-60.0, 2.0);
    CHECK(d.phaseNow() == 0);
    const std::size_t dropAt = d.rec.phase.size();
    d.level(-3.0, 8.0);
    CHECK(anyPhase(d.rec, 2, dropAt, dropAt + static_cast<std::size_t>(1.0 * kFps)));   // within 1 s
    CHECK(d.a.dropCount() == 1);
    // Drop holds for ~dropHoldS (4 s) then returns to Steady
    std::size_t first = dropAt; while (d.rec.phase[first] != 2) ++first;
    std::size_t last = first; while (last + 1 < d.rec.phase.size() && d.rec.phase[last + 1] == 2) ++last;
    CHECK(double(last - first) / kFps == doctest::Approx(4.0).epsilon(0.1));
    CHECK(d.phaseNow() == 0);
}

TEST_CASE("Energy Arc: a step at the end of a build is a drop even without a dip") {
    Driver d;
    d.level(-40.0, 15.0);
    d.ramp(-40.0, -22.0, 12.0);
    const std::size_t at = d.rec.phase.size();
    CHECK(anyPhase(d.rec, 1, at - static_cast<std::size_t>(4 * kFps), at));
    d.level(-8.0, 3.0);
    CHECK(anyPhase(d.rec, 2, at, d.rec.phase.size()));
}

TEST_CASE("Energy Arc: no false drops from beat-level fluctuation, music starting from silence, or level steps down") {
    {
        Driver d;                                                  // +-4 dB at 2 Hz plus pseudo-random +-3 dB per tick
        Rng rng(42);
        for (int i = 0; i < static_cast<int>(60 * kFps); ++i) {
            const double db = -25.0 + 4.0 * std::sin(2 * sst::kPi * 2.0 * i / kFps) + rng.range(-3.f, 3.f);
            d.level(db, 1.0 / kFps);
        }
        CHECK_FALSE(anyPhase(d.rec, 2, 0, d.rec.phase.size()));
        CHECK_FALSE(anyPhase(d.rec, 1, static_cast<std::size_t>(15 * kFps), d.rec.phase.size()));
    }
    {
        Driver d;                                                  // silence, then music starts: not a drop
        d.level(-120.0, 6.0);
        d.level(-20.0, 20.0);
        CHECK_FALSE(anyPhase(d.rec, 2, 0, d.rec.phase.size()));
    }
    {
        Driver d;                                                  // sudden step down is not a drop
        d.level(-10.0, 20.0);
        d.level(-40.0, 20.0);
        CHECK_FALSE(anyPhase(d.rec, 2, 0, d.rec.phase.size()));
    }
}

TEST_CASE("Energy Arc: refractory period prevents two drops within refractoryS") {
    Driver d;
    d.level(-12.0, 20.0);
    d.level(-60.0, 2.0); d.level(-3.0, 1.5);                       // drop 1
    d.level(-60.0, 2.0); d.level(-3.0, 1.5);                       // would be drop 2, 3.5 s later: suppressed
    CHECK(d.a.dropCount() == 1);
    d.level(-3.0, 10.0);
    d.level(-60.0, 2.0); d.level(-3.0, 3.0);                       // > 8 s after drop 1
    CHECK(d.a.dropCount() == 2);
}

TEST_CASE("Energy Arc: deterministic, silence forces level 0, hostile input is sanitised") {
    auto run = [] {
        Driver d;
        d.level(-35.0, 20.0); d.ramp(-35.0, -10.0, 12.0); d.level(-60.0, 2.0); d.level(-3.0, 6.0);
        return d.rec;
    };
    const Rec a = run(), b = run();
    CHECK(a.arc == b.arc);
    CHECK(a.phase == b.phase);
    EnergyArc e; REQUIRE(e.prepare(187.5));
    for (int i = 0; i < 5000; ++i) e.push(i % 3 == 0 ? std::nan("") : (i % 3 == 1 ? 1e300 : -5.0), false);
    CHECK(e.arc() >= 0.f); CHECK(e.arc() <= 1.f);
    EnergyArc s; REQUIRE(s.prepare(187.5));
    for (int i = 0; i < 187 * 40; ++i) s.push(0.5, true);
    CHECK(s.arc() == 0.f);
    CHECK(s.phase() == EnergyPhase::Steady);
    CHECK_FALSE(EnergyArc().prepare(1.0));
}

TEST_CASE("Energy Arc: tunables are validated; invalid sets are rejected and change nothing") {
    EnergyArc e; REQUIRE(e.prepare(187.5));
    EnergyArcParams p = e.params();
    CHECK(p.valid());
    auto bad = [&](auto mutate) {
        EnergyArcParams q = e.params(); mutate(q);
        const Status st = e.setParams(q);
        CHECK_FALSE(st.ok());
        CHECK(st.error().category == ErrorCategory::InvalidArgument);
        CHECK(e.params().fastTauS == p.fastTauS);                 // unchanged
    };
    bad([](EnergyArcParams& q) { q.levelFloorDb = 0.0; });
    bad([](EnergyArcParams& q) { q.levelFloorDb = -20.0; q.levelCeilDb = -5.0; });      // span < 20 dB
    bad([](EnergyArcParams& q) { q.fastTauS = 0.0; });
    bad([](EnergyArcParams& q) { q.arcTauS = 100.0; });
    bad([](EnergyArcParams& q) { q.buildRise = std::nan(""); });
    bad([](EnergyArcParams& q) { q.buildWindowS = 1.0; });
    bad([](EnergyArcParams& q) { q.dropJump = 5.0; });
    bad([](EnergyArcParams& q) { q.dropDipDepth = 0.0; });
    bad([](EnergyArcParams& q) { q.dropHoldS = 60.0; });
    bad([](EnergyArcParams& q) { q.refractoryS = 0.1; });
    EnergyArcParams ok = p; ok.levelFloorDb = -60.0; ok.arcTauS = 12.0; ok.dropHoldS = 2.0;
    CHECK(e.setParams(ok).ok());
    CHECK(e.params().arcTauS == 12.0);
    // the arc follows the new mapping: -30 dB with floor -60/ceil 0 -> 0.5
    for (int i = 0; i < static_cast<int>(60 * 187.5); ++i) e.push(std::pow(10.0, -3.0), false);
    CHECK(e.arc() == doctest::Approx(0.5f).epsilon(0.02));
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    EnergyArcParams q; q.dropJump = 0.0;
    CHECK_FALSE(a.setEnergyArcParams(q).ok());
    CHECK(a.setEnergyArcParams(ok).ok());
}

TEST_CASE("Energy Arc: end-to-end through the Analyzer (audio build, breakdown, drop)") {
    const std::uint32_t rate = 48000;
    auto tone = [&](double db, double sec) { return sst::sine(440.0, std::pow(10.0, db / 20.0), sec, rate); };
    Analyzer a; REQUIRE(a.prepare(rate, 1).ok());
    sst::Feeder fd(a, 1, rate);
    fd.feed(tone(-35, 22), 960);
    CHECK(a.peek().arcPhase == 0);
    CHECK(a.peek().energyArc == doctest::Approx(0.3f).epsilon(0.1));
    bool sawBuild = false, sawDrop = false;
    for (int i = 0; i < 15; ++i) {                                  // 1 s steps rising 1.6 dB each: 24 dB over 15 s
        fd.feed(tone(-35 + 1.6 * (i + 1), 1.0), 960);
        sawBuild = sawBuild || a.peek().arcPhase == 1;
    }
    CHECK(sawBuild);
    fd.feed(tone(-11, 8.0), 960);
    CHECK(a.peek().arcPhase == 0);
    fd.feed(std::vector<float>(rate * 2, 0.f), 960);                // breakdown
    for (int i = 0; i < 8; ++i) { fd.feed(tone(-3, 0.5), 960); sawDrop = sawDrop || a.peek().arcPhase == 2; }
    CHECK(sawDrop);
    const FeatureSnapshot s = a.peek();
    CHECK(s.energyArc > 0.2f);
    CHECK(s.energyArc <= 1.0f);
}
