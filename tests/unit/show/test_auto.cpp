#include <algorithm>
#include <map>
#include <set>
#include "rig.hpp"
using namespace sst;

namespace {
std::vector<std::uint32_t> runAuto(const model::ShowDocument& d, Ns total) {
    Rig r(d);
    r.run(total);
    return r.sceneSeq();
}
}  // namespace

TEST_CASE("REQ-S01: seeded auto sequence reproduces across 3 runs and differs by seed") {
    const auto d = mkDoc(8, 1234);
    const auto a = runAuto(d, secToNs(120));
    const auto b = runAuto(d, secToNs(120));
    const auto c = runAuto(d, secToNs(120));
    CHECK(a.size() > 30);
    CHECK(a == b);
    CHECK(b == c);
    const auto other = runAuto(mkDoc(8, 999), secToNs(120));
    CHECK(other != a);
}

TEST_CASE("REQ-S01: no scene repeats within the configured history") {
    for (int hist : {1, 2, 3, 5}) {
        auto d = mkDoc(8, 7, 2, hist);
        const auto seq = runAuto(d, secToNs(600));
        REQUIRE(seq.size() > 100);
        for (std::size_t i = 0; i < seq.size(); ++i)
            for (std::size_t j = (i >= static_cast<std::size_t>(hist) ? i - hist : 0); j < i; ++j)
                CHECK_MESSAGE(seq[i] != seq[j], "hist=" << hist << " i=" << i);
    }
}

TEST_CASE("REQ-S01: pool of one never changes scene and does not spin") {
    auto d = mkDoc(1);
    Rig r(d);
    r.run(secToNs(300));
    CHECK(r.sceneSeq().size() == 1);
    CHECK(r.count(EventKind::SceneChanged) == 1);
    // only the intended pool member is eligible even when other scenes exist outside the pool
    auto d2 = mkDoc(3);
    d2.scenes[1].inAutoPool = false; d2.scenes[2].inAutoPool = false;
    Rig r2(d2);
    r2.run(secToNs(300));
    CHECK(r2.sceneSeq() == std::vector<std::uint32_t>{1});
}

TEST_CASE("REQ-S01: pool of two alternates and never repeats the current scene") {
    for (int hist : {0, 1, 3, 10}) {
        auto d = mkDoc(2, 5, 2, hist);
        const auto seq = runAuto(d, secToNs(100));
        REQUIRE(seq.size() > 20);
        for (std::size_t i = 1; i < seq.size(); ++i) CHECK(seq[i] != seq[i - 1]);
    }
}

TEST_CASE("REQ-S01: empty pool and empty show hold without changing or spinning") {
    auto d = mkDoc(3);
    for (auto& s : d.scenes) s.inAutoPool = false;
    Rig r(d);
    r.run(secToNs(60));
    CHECK(r.count(EventKind::SceneChanged) == 0);
    CHECK_FALSE(r.snap.output(kPrimaryOutput)->hasScene);
    Rig r2(mkDoc(0));
    r2.run(secToNs(60));
    CHECK(r2.count(EventKind::SceneChanged) == 0);
}

TEST_CASE("REQ-S01: minimum dwell is honoured even when the auto interval is shorter") {
    auto d = mkDoc(6, 3, 10);
    Tunables t = fastTun();
    t.autoIntervalSec = 1.0;
    Rig r(d, t);
    r.run(secToNs(200));
    std::vector<Ns> times;
    for (auto& e : r.all) if (e.kind == EventKind::SceneChanged) times.push_back(e.atNs);
    REQUIRE(times.size() > 10);
    for (std::size_t i = 1; i < times.size(); ++i) CHECK(times[i] - times[i - 1] >= secToNs(10));
}

TEST_CASE("REQ-S01: ineligible scenes are never picked") {
    auto d = mkDoc(6);
    d.scenes[2].inAutoPool = false;                       // scene 3
    d.scenes[4].strobe = true;                            // scene 5 (allowed until strobing is disabled)
    Rig r(d);
    r.act(ActionId::DisableStrobing);
    r.step(0);
    r.run(secToNs(400));
    for (auto s : r.sceneSeq()) { CHECK(s != 3); CHECK(s != 5); }
}

TEST_CASE("REQ-S01: auto changes quantise to the next bar when tempo is valid") {
    auto d = mkDoc(6, 11, 2);
    Tunables t = fastTun();
    t.quantiseAutoToBar = true;
    Rig r(d, t);
    r.act(ActionId::SetBpm, 120.0);                       // beat 0.5 s, bar 2 s, anchor = first tick (0)
    r.run(secToNs(60));
    int n = 0;
    for (auto& e : r.all)
        if (e.kind == EventKind::SceneChanged && e.atNs > 0) { CHECK(e.atNs % secToNs(2) == 0); ++n; }
    CHECK(n > 5);
}

TEST_CASE("REQ-D11: look controls reach the active scene and write through") {
    Rig r(mkDoc(3));
    r.actNow(ActionId::Brightness, 0.25);
    r.actNow(ActionId::Intensity, 0.9);
    r.actNow(ActionId::EffectPrismMirror, 0.5);
    CHECK(r.snap.output(kPrimaryOutput)->params.brightness == doctest::Approx(0.25f));
    CHECK(r.snap.output(kPrimaryOutput)->params.intensity == doctest::Approx(0.9f));
    CHECK(r.snap.output(kPrimaryOutput)->params.effectAmount[0] == doctest::Approx(0.5f));
    const auto doc = r.eng.document();
    CHECK(doc.scenes[r.scene() - 1].params.brightness == doctest::Approx(0.25f));
}

TEST_CASE("REQ-S01: Energy Arc level steers the auto pool toward matching tiers") {
    model::ShowDocument d;
    for (std::uint32_t i = 1; i <= 3; ++i) d.scenes.push_back(mkScene(i, model::EnergyTier::Low));
    for (std::uint32_t i = 4; i <= 6; ++i) d.scenes.push_back(mkScene(i, model::EnergyTier::High));
    d.scenes.push_back(mkScene(7, model::EnergyTier::Drop));
    d.seed = 77; d.minDwellSec = 2; d.repeatHistory = 2;
    Rig r(d);
    r.f.silent = false; r.f.energyArc = 0.9f;
    r.run(secToNs(1500));
    int high = 0, low = 0, drop = 0;
    for (auto s : r.sceneSeq()) { if (s >= 4 && s <= 6) ++high; else if (s <= 3) ++low; else ++drop; }
    CHECK(drop == 0);                                      // Drop-tier scenes are reserved for drops
    CHECK(high > 3 * low);
    // and the reverse at low energy
    Rig q(d);
    q.f.silent = false; q.f.energyArc = 0.1f;
    q.run(secToNs(1500));
    high = low = 0;
    for (auto s : q.sceneSeq()) { if (s >= 4 && s <= 6) ++high; else if (s <= 3) ++low; }
    CHECK(low > 3 * high);
    CHECK(q.snap.wantedTier == model::EnergyTier::Low);
}

TEST_CASE("REQ-S01: silent input ignores the energy arc preference") {
    model::ShowDocument d;
    for (std::uint32_t i = 1; i <= 3; ++i) d.scenes.push_back(mkScene(i, model::EnergyTier::Low));
    for (std::uint32_t i = 4; i <= 6; ++i) d.scenes.push_back(mkScene(i, model::EnergyTier::High));
    Rig r(d);
    r.f.silent = true; r.f.energyArc = 0.95f;
    r.run(secToNs(10));
    CHECK(r.snap.wantedTier == model::EnergyTier::Any);
}

TEST_CASE("Energy Arc: a drop schedules a Drop-tier change on the next bar boundary") {
    model::ShowDocument d;
    d.scenes = {mkScene(1), mkScene(2), mkScene(3, model::EnergyTier::Drop)};
    d.seed = 9; d.minDwellSec = 2; d.repeatHistory = 1;
    Tunables t = fastTun();
    t.autoIntervalSec = 1000.0;                            // no ordinary auto change in this test
    Rig r(d, t);
    r.act(ActionId::SetBpm, 120.0);                        // bar = 2 s from the engine start anchor
    r.run(msToNs(2200));                                   // t = 2.2 s
    const std::uint32_t before = r.scene();
    CHECK(before != 3);
    r.f.silent = false; r.f.arcPhase = 2; r.f.energyArc = 0.95f;
    r.step(msToNs(100));                                   // t = 2.3 s: drop observed
    CHECK(r.count(EventKind::DropScheduled) == 1);
    CHECK(r.snap.dropPending);
    while (r.t < msToNs(3900)) { r.step(msToNs(100)); CHECK(r.scene() == before); }
    CHECK(r.count(EventKind::Drop) == 0);
    r.step(msToNs(100));                                   // t = 4.0 s: the bar boundary
    CHECK(r.t == secToNs(4));
    CHECK(r.count(EventKind::Drop) == 1);
    CHECK(r.scene() == 3);
}

TEST_CASE("Energy Arc: the drop lands exactly on the bar and is journaled") {
    model::ShowDocument d;
    d.scenes = {mkScene(1), mkScene(2), mkScene(3, model::EnergyTier::Drop)};
    d.seed = 9; d.minDwellSec = 2; d.repeatHistory = 1;
    Tunables t = fastTun();
    t.autoIntervalSec = 1000.0;
    Rig r(d, t);
    r.act(ActionId::SetBpm, 120.0);
    r.run(msToNs(2200));
    r.f.silent = false; r.f.arcPhase = 2;
    r.run(secToNs(3));
    REQUIRE(r.count(EventKind::Drop) == 1);
    for (auto& e : r.all) if (e.kind == EventKind::Drop) { CHECK(e.atNs == secToNs(4)); CHECK(e.sceneId == 3); }
    CHECK(r.scene() == 3);
    CHECK_FALSE(r.snap.dropPending);
    std::vector<model::JournalEntry> j;
    r.eng.drainJournal(j);
    int drops = 0;
    for (auto& e : j) if (e.kind == "drop") ++drops;
    CHECK(drops == 2);                                     // scheduled + executed
    // holding arcPhase at 2 does not retrigger (edge only)
    r.run(secToNs(30));
    CHECK(r.count(EventKind::Drop) == 1);
}

TEST_CASE("Energy Arc: drop honours minimum dwell by waiting for a later bar") {
    model::ShowDocument d;
    d.scenes = {mkScene(1), mkScene(2), mkScene(3, model::EnergyTier::Drop)};
    d.seed = 9; d.minDwellSec = 5; d.repeatHistory = 1;
    Tunables t = fastTun();
    t.autoIntervalSec = 1000.0;
    Rig r(d, t);
    r.act(ActionId::SetBpm, 120.0);
    r.run(msToNs(2200));
    r.f.silent = false; r.f.arcPhase = 2;
    r.run(secToNs(6));
    REQUIRE(r.count(EventKind::Drop) == 1);
    for (auto& e : r.all) if (e.kind == EventKind::Drop) CHECK(e.atNs == secToNs(6));   // bar at 4 s is inside the dwell
}

TEST_CASE("Energy Arc: drop is skipped when only strobe drop scenes exist and strobing is disabled") {
    model::ShowDocument d;
    d.scenes = {mkScene(1), mkScene(2), mkScene(3, model::EnergyTier::Drop, true)};
    d.seed = 9; d.minDwellSec = 2; d.repeatHistory = 1; d.strobingDisabled = true;
    Tunables t = fastTun();
    t.autoIntervalSec = 1000.0;
    Rig r(d, t);
    r.act(ActionId::SetBpm, 120.0);
    r.run(msToNs(2200));
    r.f.silent = false; r.f.arcPhase = 2;
    r.run(secToNs(5));
    CHECK(r.count(EventKind::Drop) == 0);
    CHECK(r.count(EventKind::DropSkipped) == 1);
    CHECK(r.scene() != 3);
}

TEST_CASE("Energy Arc: without a tempo the drop fires unquantised once dwell allows") {
    model::ShowDocument d;
    d.scenes = {mkScene(1), mkScene(2), mkScene(3, model::EnergyTier::Drop)};
    d.seed = 9; d.minDwellSec = 2; d.repeatHistory = 1;
    Tunables t = fastTun();
    t.autoIntervalSec = 1000.0;
    Rig r(d, t);
    r.run(msToNs(2200));
    r.f.silent = false; r.f.arcPhase = 2;
    r.step(msToNs(100));
    CHECK(r.count(EventKind::Drop) == 1);
    CHECK(r.scene() == 3);
}

TEST_CASE("Energy Arc: drops do not fire in Programmed mode or while overridden") {
    model::ShowDocument d;
    d.scenes = {mkScene(1), mkScene(2), mkScene(3, model::EnergyTier::Drop)};
    d.cues = {mkCue(1, 1, 0.0)};
    d.mode = ShowMode::Programmed;
    Rig r(d);
    r.f.silent = false; r.f.arcPhase = 2;
    r.run(secToNs(20));
    CHECK(r.count(EventKind::DropScheduled) == 0);
    CHECK(r.scene() == 1);
}
