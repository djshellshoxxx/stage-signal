#include "rig.hpp"
#include "ss/show/beat_grid.hpp"
using namespace sst;

namespace {
Tunables quiet() { auto t = fastTun(); t.autoIntervalSec = 100000.0; return t; }
}

TEST_CASE("Undo/Redo: look edits coalesce, undo and redo restore exact params") {
    Rig r(mkDoc(3), quiet());
    const auto start = r.snap.output(kPrimaryOutput)->params;
    r.act(ActionId::Brightness, 0.2); r.step(msToNs(100));
    r.act(ActionId::Brightness, 0.3); r.step(msToNs(100));
    r.act(ActionId::Brightness, 0.4); r.step(msToNs(100));
    CHECK(r.snap.undoDepth == 1);                          // a knob sweep is one undo step
    CHECK(r.snap.output(kPrimaryOutput)->params.brightness == doctest::Approx(0.4f));
    r.act(ActionId::Intensity, 0.1); r.step(msToNs(100));
    CHECK(r.snap.undoDepth == 2);
    r.step(secToNs(5));
    r.act(ActionId::Brightness, 0.9); r.step(0);
    CHECK(r.snap.undoDepth == 3);                          // outside the coalescing window: new step

    CHECK(r.act(ActionId::Undo).accepted); r.step(0);
    CHECK(r.snap.output(kPrimaryOutput)->params.brightness == doctest::Approx(0.4f));
    CHECK(r.snap.redoDepth == 1);
    r.act(ActionId::Undo); r.step(0);
    CHECK(r.snap.output(kPrimaryOutput)->params.intensity == doctest::Approx(start.intensity));
    r.act(ActionId::Undo); r.step(0);
    CHECK(r.snap.output(kPrimaryOutput)->params == start);
    CHECK(r.eng.document().scenes[r.scene() - 1].params == start);   // write-through is undone too
    CHECK(r.snap.undoDepth == 0);
    CHECK_FALSE(r.act(ActionId::Undo).accepted);

    r.act(ActionId::Redo); r.step(0);
    CHECK(r.snap.output(kPrimaryOutput)->params.brightness == doctest::Approx(0.4f));
    r.act(ActionId::Redo); r.act(ActionId::Redo); r.step(0);
    CHECK(r.snap.output(kPrimaryOutput)->params.brightness == doctest::Approx(0.9f));
    CHECK(r.snap.output(kPrimaryOutput)->params.intensity == doctest::Approx(0.1f));
    CHECK(r.snap.redoDepth == 0);

    r.act(ActionId::Undo); r.step(0);
    r.act(ActionId::Speed, 0.5); r.step(0);
    CHECK(r.snap.redoDepth == 0);                          // a new edit clears redo
}

TEST_CASE("Undo/Redo: live performance actions are not on the undo stack") {
    Rig r(mkDoc(8), quiet());
    r.act(ActionId::SceneSelect, 3); r.step(0);
    r.act(ActionId::NextScene); r.step(0);
    r.act(ActionId::BankNext); r.step(0);
    r.act(ActionId::Pause); r.step(0);
    r.act(ActionId::Resume); r.step(0);
    r.act(ActionId::EmergencyBlackout); r.step(0);
    r.act(ActionId::RestoreBlackout); r.step(0);
    r.act(ActionId::SetBpm, 128); r.step(0);
    r.act(ActionId::DisableStrobing); r.step(0);
    CHECK(r.snap.undoDepth == 0);
    CHECK_FALSE(r.act(ActionId::Undo).accepted);
}

TEST_CASE("Undo/Redo: history depth is capped at 100") {
    Rig r(mkDoc(3), quiet());
    for (int i = 0; i < 130; ++i) {
        r.step(secToNs(1));                                // each edit outside the coalescing window
        r.act(ActionId::ColorHue, (i % 100) / 100.0 + 0.001);
        r.step(0);
    }
    CHECK(r.snap.undoDepth == Limits::kHistoryDepth);
}

TEST_CASE("Undo/Redo: mode changes are undoable") {
    Rig r(mkDoc(3), quiet());
    r.act(ActionId::SetMode, 2); r.step(0);
    CHECK(r.snap.mode == ShowMode::Hybrid);
    r.act(ActionId::Undo); r.step(0);
    CHECK(r.snap.mode == ShowMode::Auto);
    r.act(ActionId::Redo); r.step(0);
    CHECK(r.snap.mode == ShowMode::Hybrid);
}

TEST_CASE("Undo/Redo: pool membership edits are undoable and change auto eligibility") {
    Rig r(mkDoc(4), quiet());
    EditCommand e; e.kind = EditKind::SetPoolMembership; e.sceneId = 3; e.inPool = false;
    CHECK(r.eng.submitEdit(e).accepted);
    r.step(0);
    CHECK_FALSE(r.eng.document().scenes[2].inAutoPool);
    CHECK(r.snap.undoDepth == 1);
    r.act(ActionId::Undo); r.step(0);
    CHECK(r.eng.document().scenes[2].inAutoPool);
    r.act(ActionId::Redo); r.step(0);
    CHECK_FALSE(r.eng.document().scenes[2].inAutoPool);
    e.sceneId = 99;
    auto bad = r.eng.submitEdit(e);
    CHECK_FALSE(bad.accepted);
    CHECK(bad.reason == "unknown scene");
}

TEST_CASE("Undo/Redo: cue list edits are undoable and keep the current cue pointing at the same cue") {
    auto d = mkDoc(4);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 0.0), mkCue(2, 2, 0.0), mkCue(3, 3, 0.0)};
    Rig r(d, quiet());
    r.act(ActionId::GoToCue, 1); r.step(0);
    REQUIRE(r.snap.cueIndex == 1);

    EditCommand ins; ins.kind = EditKind::InsertCue; ins.index = 0; ins.cue = mkCue(9, 4, 5.0);
    CHECK(r.eng.submitEdit(ins).accepted); r.step(0);
    CHECK(r.eng.document().cues.size() == 4);
    CHECK(r.eng.document().cues[0].id == 9);
    CHECK(r.snap.cueIndex == 2);                           // still on cue id 2
    CHECK(r.snap.cueCount == 4);

    EditCommand mv; mv.kind = EditKind::MoveCue; mv.index = 0; mv.toIndex = 3;
    CHECK(r.eng.submitEdit(mv).accepted); r.step(0);
    CHECK(r.eng.document().cues[3].id == 9);
    CHECK(r.snap.cueIndex == 1);

    EditCommand rep; rep.kind = EditKind::ReplaceCue; rep.index = 3; rep.cue = mkCue(9, 1, 7.0);
    CHECK(r.eng.submitEdit(rep).accepted); r.step(0);
    CHECK(r.eng.document().cues[3].duration == doctest::Approx(7.0));

    EditCommand rm; rm.kind = EditKind::RemoveCue; rm.index = 0;
    CHECK(r.eng.submitEdit(rm).accepted); r.step(0);
    CHECK(r.eng.document().cues.size() == 3);
    CHECK(r.snap.cueIndex == 0);

    for (int i = 0; i < 4; ++i) { r.act(ActionId::Undo); r.step(0); }
    CHECK(r.eng.document().cues.size() == 3);
    CHECK(r.eng.document().cues[0].id == 1);
    CHECK(r.eng.document().cues[1].id == 2);
    CHECK(r.eng.document().cues[2].id == 3);
    CHECK(r.snap.cueIndex == 1);
    for (int i = 0; i < 4; ++i) { r.act(ActionId::Redo); r.step(0); }
    CHECK(r.eng.document().cues.size() == 3);
    CHECK(r.eng.document().cues[2].id == 9);
    CHECK(r.eng.document().cues[2].duration == doctest::Approx(7.0));
    CHECK(r.snap.cueIndex == 0);
}

TEST_CASE("Undo/Redo: cue edits validate with reasons") {
    auto d = mkDoc(3);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 1.0)};
    Rig r(d, quiet());
    EditCommand e; e.kind = EditKind::InsertCue; e.index = 5; e.cue = mkCue(2, 1, 1.0);
    CHECK(r.eng.submitEdit(e).reason == "cue index out of range");
    e.index = 0; e.cue = mkCue(2, 99, 1.0);
    CHECK(r.eng.submitEdit(e).reason == "cue references a missing scene");
    e.cue = mkCue(1, 1, 1.0);
    CHECK(r.eng.submitEdit(e).reason == "duplicate cue id");
    e.cue = mkCue(2, 1, -1.0);
    CHECK(r.eng.submitEdit(e).reason == "cue duration out of range");
    e.kind = EditKind::RemoveCue; e.index = 4;
    CHECK(r.eng.submitEdit(e).reason == "cue index out of range");
    e.kind = EditKind::MoveCue; e.index = 0; e.toIndex = 1;
    CHECK(r.eng.submitEdit(e).reason == "cue index out of range");
}

TEST_CASE("A/B snapshots: store captures the look, recall restores it and is itself undoable") {
    Rig r(mkDoc(4), quiet());
    r.act(ActionId::SceneSelect, 1); r.step(0);                  // scene 2
    r.act(ActionId::Brightness, 0.3); r.act(ActionId::EffectPrismMirror, 0.7); r.step(0);
    const auto lookA = r.snap.output(kPrimaryOutput)->params;
    CHECK(r.act(ActionId::SnapshotStoreA).accepted); r.step(0);
    CHECK(r.snap.slots[0].used);
    CHECK(r.snap.slots[0].sceneId == 2);
    CHECK_FALSE(r.snap.slots[1].used);

    r.act(ActionId::SceneSelect, 3); r.step(0);                  // scene 4
    r.step(secToNs(5));
    r.act(ActionId::Brightness, 1.0); r.act(ActionId::EffectPrismMirror, 0.0); r.step(0);
    const auto lookB = r.snap.output(kPrimaryOutput)->params;
    CHECK(r.act(ActionId::SnapshotStoreB).accepted); r.step(0);
    CHECK(r.snap.slots[1].sceneId == 4);
    const auto depthBefore = r.snap.undoDepth;

    r.act(ActionId::SnapshotRecallA); r.step(0);
    CHECK(r.scene() == 2);
    CHECK(r.snap.output(kPrimaryOutput)->params == lookA);
    CHECK(r.snap.undoDepth == depthBefore + 1);
    CHECK(r.count(EventKind::SnapshotRecalled) == 1);
    std::vector<model::JournalEntry> j;
    r.eng.drainJournal(j);
    bool journaled = false;
    for (auto& e : j) journaled = journaled || e.kind == "snapshot";
    CHECK(journaled);

    r.act(ActionId::Undo); r.step(0);
    CHECK(r.scene() == 4);
    CHECK(r.snap.output(kPrimaryOutput)->params == lookB);
    r.act(ActionId::Redo); r.step(0);
    CHECK(r.scene() == 2);
    CHECK(r.snap.output(kPrimaryOutput)->params == lookA);
    r.act(ActionId::SnapshotRecallB); r.step(0);
    CHECK(r.scene() == 4);
    CHECK(r.snap.output(kPrimaryOutput)->params == lookB);
}

TEST_CASE("A/B snapshots: recall refuses strobe scenes while strobing is disabled") {
    auto d = mkDoc(3);
    d.scenes[1].strobe = true;
    Rig r(d, quiet());
    r.act(ActionId::SceneSelect, 1); r.step(0);
    r.act(ActionId::SnapshotStoreA); r.step(0);
    r.act(ActionId::SceneSelect, 0); r.step(0);
    r.actNow(ActionId::DisableStrobing);
    auto res = r.act(ActionId::SnapshotRecallA);
    CHECK_FALSE(res.accepted);
    CHECK(res.reason == "strobing disabled");
}

TEST_CASE("REQ-A06: SetBpm and TapTempo override estimates until released") {
    Rig r(mkDoc(3), quiet());
    r.f.tempo.bpm = 100.0; r.f.tempo.source = ValueSource::Estimated; r.f.tempo.confidence = 0.8f;
    r.step(0);
    CHECK(r.snap.tempo.bpm == doctest::Approx(100.0));
    CHECK(r.snap.tempo.source == ValueSource::Estimated);
    CHECK_FALSE(r.snap.tempoOverride);

    r.actNow(ActionId::SetBpm, 140.0);
    CHECK(r.snap.tempo.bpm == doctest::Approx(140.0));
    CHECK(r.snap.tempo.source == ValueSource::Manual);
    CHECK(r.snap.tempoOverride);
    r.f.tempo.bpm = 90.0; r.step(msToNs(100));
    CHECK(r.snap.tempo.bpm == doctest::Approx(140.0));     // detector updates do not override the operator

    for (int i = 0; i < 5; ++i) { r.step(msToNs(500)); r.act(ActionId::TapTempo); r.step(0); }
    CHECK(r.snap.tempo.source == ValueSource::Tap);
    CHECK(r.snap.tempo.bpm == doctest::Approx(120.0).epsilon(0.001));

    r.actNow(ActionId::ReleaseBpmOverride);
    CHECK_FALSE(r.snap.tempoOverride);
    CHECK(r.snap.tempo.bpm == doctest::Approx(90.0));
    CHECK(r.snap.tempo.source == ValueSource::Estimated);
    CHECK(r.count(EventKind::TempoOverrideChanged) >= 6);
}

TEST_CASE("REQ-A06: tap tempo is deterministic, resets after a pause and honours input timestamps") {
    Rig r(mkDoc(3), quiet());
    auto tapAt = [&](Ns ts) { r.act(ActionId::TapTempo, 0, kAllOutputs, TriggerKind::Immediate, ts); r.step(0); };
    tapAt(secToNs(10)); CHECK_FALSE(r.snap.tempoOverride);            // one tap is not a tempo
    tapAt(secToNs(10) + msToNs(600)); CHECK(r.snap.tempo.bpm == doctest::Approx(100.0).epsilon(0.001));
    tapAt(secToNs(10) + msToNs(1200)); CHECK(r.snap.tempo.bpm == doctest::Approx(100.0).epsilon(0.001));
    tapAt(secToNs(20)); CHECK(r.snap.tempo.bpm == doctest::Approx(100.0).epsilon(0.001));   // gap: new sequence, old value kept
    tapAt(secToNs(20) + msToNs(500)); CHECK(r.snap.tempo.bpm == doctest::Approx(120.0).epsilon(0.001));
    tapAt(secToNs(20) + msToNs(500));                                 // duplicate timestamp ignored
    CHECK(r.snap.tempo.bpm == doctest::Approx(120.0).epsilon(0.001));
}

TEST_CASE("REQ-A08: manual key overrides the detector until released") {
    Rig r(mkDoc(3), quiet());
    r.f.key.tonic = 2; r.f.key.minor = false; r.f.key.source = ValueSource::Estimated; r.f.key.confidence = 0.6f;
    r.step(0);
    CHECK(r.snap.key.tonic == 2);
    r.actNow(ActionId::SetKey, 9 + 12);                    // A minor
    CHECK(r.snap.key.tonic == 9);
    CHECK(r.snap.key.minor);
    CHECK(r.snap.key.source == ValueSource::Manual);
    CHECK(r.snap.keyOverride);
    r.actNow(ActionId::ReleaseKeyOverride);
    CHECK(r.snap.key.tonic == 2);
    CHECK_FALSE(r.snap.keyOverride);
}

TEST_CASE("quantised triggers use the active tempo: NextBeat and NextBar boundaries") {
    Rig r(mkDoc(6), quiet());
    r.actNow(ActionId::SetBpm, 120.0);                     // beat 500 ms, bar 2 s, anchor 0
    r.run(msToNs(300));
    const auto s0 = r.scene();
    CHECK(r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::NextBeat).accepted);
    r.step(msToNs(100));                                   // 0.4 s
    CHECK(r.scene() == s0);
    CHECK(r.eng.stats().pending == 1);
    r.step(msToNs(100));                                   // 0.5 s: beat boundary
    CHECK(r.scene() != s0);
    CHECK(r.eng.stats().pending == 0);
    const Ns changedAt = r.t;
    CHECK(changedAt == msToNs(500));

    const auto s1 = r.scene();
    CHECK(r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::NextBar).accepted);
    r.run(msToNs(1400));                                   // 1.9 s
    CHECK(r.scene() == s1);
    r.step(msToNs(100));                                   // 2.0 s
    CHECK(r.scene() != s1);
}

TEST_CASE("quantised triggers: bars count from the last operator downbeat, not an assumed one") {
    Rig r(mkDoc(6), quiet());
    r.actNow(ActionId::SetBpm, 120.0);
    r.run(msToNs(300));
    r.eng.markDownbeat(msToNs(700));                       // operator says 0.7 s is beat 1 of a bar
    const auto s0 = r.scene();
    r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::NextBar);
    r.run(msToNs(1900));                                   // t = 2.2 s, next bar after 0.3 s is 0.7 s? (strictly after submit)
    CHECK(r.scene() != s0);                                // fired at 0.7 s, long before the engine-start bar at 2 s
    CHECK(r.snap.beatInBar >= 0);
    Ns firedAt = -1;
    for (auto& e : r.all) if (e.kind == EventKind::SceneChanged && e.atNs > 0) { firedAt = e.atNs; break; }
    CHECK(firedAt == msToNs(700));
}

TEST_CASE("quantised triggers: tempo changes keep beat phase") {
    BeatGrid g(4);
    g.setDownbeat(0);
    CHECK(g.setTempo(120.0, 0, 0.05));
    CHECK(g.boundaryAfter(msToNs(100), false) == msToNs(500));
    CHECK(g.boundaryAfter(msToNs(100), true) == msToNs(2000));
    CHECK(g.setTempo(60.0, msToNs(1250), 0.05));           // re-phased on the last 120 bpm beat (1.0 s, beat index 2)
    CHECK(g.beatAt(msToNs(1250)) == 2);
    CHECK(g.boundaryAfter(msToNs(1250), false) == msToNs(2000));
    CHECK(g.boundaryAfter(msToNs(1250), true) == msToNs(3000));   // new anchor 1.0 s is beat 2: bar line falls at 3.0 s
    CHECK_FALSE(g.setTempo(60.02, msToNs(1300), 0.05));    // jitter inside epsilon never moves the grid
    CHECK(g.beatInBar(msToNs(1250)) == 2);
}

TEST_CASE("quantised triggers: AtTime fires at the given time and a lost tempo falls back") {
    Rig r(mkDoc(6), quiet());
    const auto s0 = r.scene();
    CHECK(r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::AtTime, secToNs(3)).accepted);
    r.run(msToNs(2900));
    CHECK(r.scene() == s0);
    r.step(msToNs(100));
    CHECK(r.scene() != s0);

    Rig q(mkDoc(6), quiet());
    q.f.tempo.bpm = 120; q.f.tempo.source = ValueSource::Estimated;
    q.step(msToNs(100));
    const auto q0 = q.scene();
    CHECK(q.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::NextBar).accepted);
    q.f.tempo = TempoState{};                              // tempo vanishes before the bar
    q.run(msToNs(3800));
    CHECK(q.scene() == q0);
    q.run(msToNs(300));                                    // 4 s fallback wait elapsed
    CHECK(q.scene() != q0);
}

TEST_CASE("IF-06: independent outputs hold separate scene state, duplicates mirror their source") {
    auto d = mkDoc(6);
    model::OutputDef o2; o2.id = 2; o2.kind = RouteKind::Independent;
    model::OutputDef o3; o3.id = 3; o3.kind = RouteKind::Duplicate; o3.duplicateOf = 1;
    model::OutputDef o4; o4.id = 4; o4.kind = RouteKind::Duplicate; o4.duplicateOf = 2;
    d.outputs = {model::OutputDef{}, o2, o3, o4};
    Rig r(d, quiet());
    r.act(ActionId::SceneSelect, 4, 2); r.step(0);          // only output 2
    CHECK(r.snap.output(2)->sceneId == 5);
    CHECK(r.snap.output(1)->sceneId != 5);
    CHECK_FALSE(r.snap.manualOverride);                    // automation never drove output 2
    CHECK(r.snap.output(3)->mirrorOf == 1);
    CHECK(r.snap.output(3)->sceneId == r.snap.output(1)->sceneId);
    CHECK(r.snap.output(4)->mirrorOf == 2);
    CHECK(r.snap.output(4)->sceneId == 5);
    CHECK(r.snap.output(2)->mirrorOf == 0);

    r.act(ActionId::SceneSelect, 1, 3); r.step(0);          // target a duplicate: goes to its source
    CHECK(r.snap.output(1)->sceneId == 2);
    CHECK(r.snap.output(3)->sceneId == 2);
    CHECK(r.snap.output(2)->sceneId == 5);
    CHECK(r.snap.manualOverride);                          // primary was touched

    r.act(ActionId::Brightness, 0.1, 2); r.step(0);
    CHECK(r.snap.output(2)->params.brightness == doctest::Approx(0.1f));
    CHECK(r.snap.output(1)->params.brightness != doctest::Approx(0.1f));
    CHECK(r.snap.output(4)->params.brightness == doctest::Approx(0.1f));

    r.act(ActionId::SceneSelect, 0); r.step(0);             // all outputs
    CHECK(r.snap.output(1)->sceneId == 1);
    CHECK(r.snap.output(2)->sceneId == 1);
    auto bad = r.act(ActionId::SceneSelect, 0, 9);
    CHECK_FALSE(bad.accepted);
    CHECK(bad.reason == "unknown output");
}

TEST_CASE("IF-06: SelectOutput records the operator's selected target") {
    auto d = mkDoc(3);
    model::OutputDef o2; o2.id = 2; o2.kind = RouteKind::Independent;
    d.outputs = {model::OutputDef{}, o2};
    Rig r(d, quiet());
    CHECK(r.snap.selectedOutput == kAllOutputs);
    r.actNow(ActionId::SelectOutput, 2);
    CHECK(r.snap.selectedOutput == 2);
    r.actNow(ActionId::SelectOutput, -1);
    CHECK(r.snap.selectedOutput == kAllOutputs);
}

TEST_CASE("REQ-D11: scene banks page through 8-pad banks") {
    Rig r(mkDoc(20), quiet());
    CHECK(r.snap.bankCount == 3);
    r.actNow(ActionId::BankNext);
    CHECK(r.snap.bank == 1);
    r.act(ActionId::SceneSelect, 2); r.step(0);
    CHECK(r.scene() == 11);
    r.actNow(ActionId::BankNext); r.actNow(ActionId::BankNext);
    CHECK(r.snap.bank == 0);                               // wraps
    r.actNow(ActionId::BankPrev);
    CHECK(r.snap.bank == 2);
    CHECK_FALSE(r.act(ActionId::SceneSelect, 5).accepted); // bank 2 holds scenes 17..20 only
}

TEST_CASE("event journal: bounded queue drains, sink receives entries, JournalEntry-compatible") {
    Rig r(mkDoc(4), quiet());
    r.actNow(ActionId::EmergencyBlackout);
    std::vector<model::JournalEntry> j;
    CHECK(r.eng.drainJournal(j) >= 2);
    bool sawSafety = false, sawScene = false;
    for (auto& e : j) {
        sawSafety = sawSafety || (e.kind == "safety" && e.detail.find("BlackoutChanged") == 0);
        sawScene = sawScene || e.kind == "scene";
        CHECK(e.wallUnixMs > 0);
    }
    CHECK(sawSafety);
    CHECK(sawScene);

    std::vector<model::JournalEntry> got;
    r.eng.setJournalSink([&](const model::JournalEntry& e) { got.push_back(e); });
    r.actNow(ActionId::RestoreBlackout);
    CHECK_FALSE(got.empty());
    CHECK(r.eng.drainJournal(j) == 0);                     // with a sink, nothing is queued

    // overflow of the queue drops newest and counts
    Rig o(mkDoc(2), quiet());
    for (int i = 0; i < 700; ++i) { o.act(ActionId::EmergencyBlackout); o.step(0); o.act(ActionId::RestoreBlackout); o.step(0); }
    CHECK(o.eng.stats().journalDropped > 0);
    std::vector<model::JournalEntry> all;
    CHECK(o.eng.drainJournal(all) == Limits::kJournalCapacity);
}

TEST_CASE("events per tick are bounded and the overflow is counted") {
    Rig r(mkDoc(2), quiet());
    for (int i = 0; i < 100; ++i) { r.act(ActionId::EmergencyBlackout); r.act(ActionId::RestoreBlackout); }
    const auto s = r.eng.tick(r.t + 1, r.f);
    CHECK(s.events.size() == Limits::kEventsPerTick);
    CHECK(s.eventsDropped == 200 - Limits::kEventsPerTick);
}

TEST_CASE("tick is monotonic even when handed an older timestamp") {
    Rig r(mkDoc(2), quiet());
    r.step(secToNs(5));
    const auto s = r.eng.tick(secToNs(1), r.f);
    CHECK(s.atNs == secToNs(5));
}

TEST_CASE("BeatGrid: floor division and bar boundaries for negative offsets") {
    BeatGrid g(4);
    g.setDownbeat(secToNs(10));
    g.setTempo(120.0, secToNs(10), 0.05);
    CHECK(g.boundaryAfter(secToNs(9), true) == secToNs(10));        // before the anchor
    CHECK(g.boundaryAfter(secToNs(10), true) == secToNs(12));       // strictly after
    CHECK(g.barAt(secToNs(14)) == 2);
    CHECK(g.beatInBar(msToNs(10500)) == 1);
    CHECK(BeatGrid::floorDiv(-1, 4) == -1);
    CHECK(BeatGrid::mod(-1, 4) == 3);
}
