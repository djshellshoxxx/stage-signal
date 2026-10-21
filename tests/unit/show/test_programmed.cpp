#include "rig.hpp"
using namespace sst;

namespace {
model::ShowDocument progDoc() {
    auto d = mkDoc(4);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(10, 1, 2.0), mkCue(11, 2, 3.0), mkCue(12, 3, 4.0), mkCue(13, 4, 0.0)};
    return d;
}
std::vector<std::pair<int, Ns>> cueTimes(const Rig& r) {
    std::vector<std::pair<int, Ns>> v;
    for (auto& e : r.all) if (e.kind == EventKind::CueFired) v.push_back({e.cueIndex, e.atNs});
    return v;
}
}  // namespace

TEST_CASE("REQ-S02: programmed cues execute in declared order with seconds timing") {
    Rig r(progDoc());
    r.run(secToNs(20));
    const auto v = cueTimes(r);
    REQUIRE(v.size() == 4);
    CHECK(v[0] == std::make_pair(0, Ns(0)));
    CHECK(v[1] == std::make_pair(1, secToNs(2)));
    CHECK(v[2] == std::make_pair(2, secToNs(5)));
    CHECK(v[3] == std::make_pair(3, secToNs(9)));
    CHECK(r.scene() == 4);
    CHECK(r.snap.cueHolding);                              // duration 0 = hold until trigger
    CHECK(r.snap.cueIndex == 3);
}

TEST_CASE("REQ-S02: cue carries transition and next-scene lookahead") {
    auto d = progDoc();
    d.cues[1].transition = model::TransitionKind::Crossfade; d.cues[1].transitionDuration = 1.0;
    Rig r(d);
    CHECK(r.snap.output(kPrimaryOutput)->hasNext);
    CHECK(r.snap.output(kPrimaryOutput)->nextSceneId == 2);
    r.run(secToNs(2));
    r.run(msToNs(500));                                    // 0.5 s into a 1 s crossfade
    const auto* o = r.snap.output(kPrimaryOutput);
    CHECK(o->sceneId == 2);
    CHECK(o->hasFrom);
    CHECK(o->fromSceneId == 1);
    CHECK(o->transitionProgress == doctest::Approx(0.5));
    r.run(secToNs(1));
    CHECK(r.snap.output(kPrimaryOutput)->hasFrom == false);
    CHECK(r.snap.output(kPrimaryOutput)->transitionProgress == doctest::Approx(1.0));
}

TEST_CASE("REQ-S02: beat-based cues follow the active tempo") {
    auto d = mkDoc(3);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 4.0, model::TimeBase::Beats), mkCue(2, 2, 4.0, model::TimeBase::Beats), mkCue(3, 3, 0.0)};
    Rig r(d);
    r.f.tempo.bpm = 120.0; r.f.tempo.source = ValueSource::Estimated; r.f.tempo.confidence = 0.9f;
    r.run(secToNs(10));
    const auto v = cueTimes(r);
    REQUIRE(v.size() == 3);
    CHECK(v[1].second >= secToNs(2));
    CHECK(v[1].second <= secToNs(2) + msToNs(100));        // 4 beats at 120 bpm = 2 s
    CHECK(v[2].second >= secToNs(4));
    CHECK(v[2].second <= secToNs(4) + msToNs(200));
}

TEST_CASE("REQ-S02: beat cues wait, not guess, when no tempo is available") {
    auto d = mkDoc(2);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 2.0, model::TimeBase::Beats), mkCue(2, 2, 0.0)};
    Rig r(d);
    r.run(secToNs(30));
    CHECK(r.snap.cueIndex == 0);
    CHECK(r.snap.cueWaitingForTempo);
    r.f.tempo.bpm = 60.0; r.f.tempo.source = ValueSource::Tap;
    r.run(secToNs(3));
    CHECK(r.snap.cueIndex == 1);
}

TEST_CASE("REQ-S02: waitForTrigger cues hold until an explicit Next (band mode)") {
    auto d = mkDoc(3);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 1.0), mkCue(2, 2, 1.0, model::TimeBase::Seconds, true), mkCue(3, 3, 1.0)};
    Rig r(d);
    r.run(secToNs(30));
    CHECK(r.snap.cueIndex == 0);
    CHECK(r.snap.cueHolding);
    CHECK(r.act(ActionId::NextCue).accepted);
    r.run(msToNs(100));
    CHECK(r.snap.cueIndex == 1);
    r.run(secToNs(2));                                     // cue 2 has no wait flag: follows automatically
    CHECK(r.snap.cueIndex == 2);
}

TEST_CASE("REQ-S02: next, previous and go-to cue; missing cue is rejected with a reason") {
    Rig r(progDoc());
    CHECK(r.act(ActionId::NextCue).accepted);
    r.step(0);
    CHECK(r.snap.cueIndex == 1);
    CHECK(r.act(ActionId::PrevCue).accepted);
    r.step(0);
    CHECK(r.snap.cueIndex == 0);
    CHECK(r.act(ActionId::GoToCue, 3).accepted);
    r.step(0);
    CHECK(r.snap.cueIndex == 3);
    auto bad = r.act(ActionId::GoToCue, 9);
    CHECK_FALSE(bad.accepted);
    CHECK(bad.reason == "cue index out of range");
    CHECK_FALSE(r.act(ActionId::NextCue).accepted);        // already on the last cue
    CHECK_FALSE(r.act(ActionId::GoToCue, 1.5).accepted);
    r.step(0);
    r.act(ActionId::GoToCue, 0); r.step(0);
    CHECK_FALSE(r.act(ActionId::PrevCue).accepted);        // on the first cue
}

TEST_CASE("REQ-S02: cue actions need Programmed mode") {
    Rig r(mkDoc(3));
    auto res = r.act(ActionId::NextCue);
    CHECK_FALSE(res.accepted);
    CHECK(res.reason == "cue actions require Programmed mode");
}

TEST_CASE("REQ-S02: a document whose cue references a missing scene is rejected") {
    auto d = progDoc();
    d.cues[2].sceneId = 999;
    ShowEngine e;
    CHECK_FALSE(e.loadDocument(d).ok());
}

TEST_CASE("REQ-S02: cue list end is reported once, loop restarts when enabled") {
    auto d = mkDoc(2);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 1.0), mkCue(2, 2, 1.0)};
    Rig r(d);
    r.run(secToNs(10));
    CHECK(r.count(EventKind::CueListEnded) == 1);
    CHECK(r.snap.programEnded);
    Tunables t = fastTun(); t.loopCues = true;
    Rig l(d, t);
    l.run(secToNs(10));
    CHECK(l.count(EventKind::CueListEnded) == 0);
    CHECK(l.count(EventKind::CueFired) >= 9);
}

TEST_CASE("REQ-S03: manual scene selection overrides; no automatic cue fires until Resume") {
    Rig r(progDoc());
    r.run(msToNs(500));
    CHECK(r.snap.cueIndex == 0);
    CHECK(r.act(ActionId::SceneSelect, 3).accepted);       // pad 4 (slot 3)
    r.step(0);
    CHECK(r.snap.manualOverride);
    CHECK(r.scene() == 4);
    const int firedBefore = r.count(EventKind::CueFired);
    r.run(secToNs(120));
    CHECK(r.count(EventKind::CueFired) == firedBefore);
    CHECK(r.snap.cueIndex == 0);
    CHECK(r.scene() == 4);
    CHECK(r.act(ActionId::Resume, 0).accepted);
    r.step(0);
    CHECK_FALSE(r.snap.manualOverride);
    CHECK(r.scene() == 4);                                 // continue = no surprise jump of the picture
    r.run(secToNs(2));
    CHECK(r.snap.cueIndex == 1);                           // cue 0 had 0.5 s elapsed: 1.5 s remained
}

TEST_CASE("REQ-S03: manual selection in Auto becomes Hybrid override and freezes auto changes") {
    Rig r(mkDoc(6));
    r.run(secToNs(1));
    CHECK(r.act(ActionId::SceneSelect, 4).accepted);
    r.step(0);
    CHECK(r.snap.mode == ShowMode::Hybrid);
    CHECK(r.snap.manualOverride);
    const auto sel = r.scene();
    CHECK(sel == 5);
    r.run(secToNs(300));
    CHECK(r.scene() == sel);
    r.act(ActionId::Resume); r.step(0);
    r.run(secToNs(10));
    CHECK(r.scene() != sel);                               // automation is live again
    CHECK(r.snap.mode == ShowMode::Hybrid);
}

TEST_CASE("REQ-S03: explicit cue trigger still works while overridden but automation stays frozen") {
    Rig r(progDoc());
    r.act(ActionId::SceneSelect, 3); r.step(0);
    r.act(ActionId::NextCue); r.step(0);
    CHECK(r.snap.cueIndex == 1);
    r.run(secToNs(60));
    CHECK(r.snap.cueIndex == 1);
    CHECK(r.snap.manualOverride);
}

TEST_CASE("REQ-S04: Pause keeps cue position; Resume continues without a jump") {
    Rig r(progDoc());
    r.run(msToNs(3500));                                   // cue 1 started at 2 s: 1.5 s elapsed
    REQUIRE(r.snap.cueIndex == 1);
    r.act(ActionId::Pause); r.step(0);
    CHECK(r.snap.paused);
    const double elapsed = r.snap.cueElapsedSec;
    r.run(secToNs(100));
    CHECK(r.snap.cueIndex == 1);
    CHECK(r.snap.cueElapsedSec == doctest::Approx(elapsed));
    r.act(ActionId::Resume, 0); r.step(0);
    CHECK_FALSE(r.snap.paused);
    CHECK(r.snap.cueIndex == 1);
    r.run(msToNs(1400));
    CHECK(r.snap.cueIndex == 1);                           // 1.5 s still remain
    r.run(msToNs(200));
    CHECK(r.snap.cueIndex == 2);
}

TEST_CASE("REQ-S04: Resume can advance to the next cue explicitly") {
    Rig r(progDoc());
    r.run(msToNs(500));
    r.act(ActionId::Pause); r.step(0);
    r.run(secToNs(10));
    CHECK(r.snap.cueIndex == 0);
    CHECK(r.act(ActionId::Resume, 1).accepted);
    r.step(0);
    CHECK(r.snap.cueIndex == 1);
    CHECK_FALSE(r.snap.paused);
    CHECK_FALSE(r.act(ActionId::Resume, 2).accepted);
}

TEST_CASE("REQ-S04: Auto dwell is frozen by Pause and Resume offers advance") {
    Rig r(mkDoc(6, 3, 2));
    r.run(msToNs(1500));
    const auto first = r.scene();
    r.act(ActionId::Pause); r.step(0);
    r.run(secToNs(60));
    CHECK(r.scene() == first);
    r.act(ActionId::Resume, 1); r.step(0);
    CHECK(r.scene() != first);
}

TEST_CASE("REQ-S03: mode changes through SetMode and clear a stale override") {
    Rig r(progDoc());
    r.act(ActionId::SceneSelect, 2); r.step(0);
    CHECK(r.snap.manualOverride);
    CHECK(r.act(ActionId::SetMode, 0).accepted);
    r.step(0);
    CHECK(r.snap.mode == ShowMode::Auto);
    CHECK_FALSE(r.snap.manualOverride);
    CHECK_FALSE(r.act(ActionId::SetMode, 7).accepted);
    r.act(ActionId::SetMode, 1); r.step(0);
    CHECK(r.snap.mode == ShowMode::Programmed);
}
