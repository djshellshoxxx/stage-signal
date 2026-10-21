#include <cmath>
#include "rig.hpp"
using namespace sst;

TEST_CASE("REQ-D08: blackout latches are separate, explicit restore, scene state retained") {
    Rig r(mkDoc(4));
    r.act(ActionId::SceneSelect, 2); r.step(0);
    const auto scene = r.scene();
    const auto params = r.snap.output(kPrimaryOutput)->params;

    r.act(ActionId::VisualBlackout); r.step(0);
    CHECK(r.snap.visualBlackout);
    CHECK_FALSE(r.snap.lightingBlackout);
    CHECK_FALSE(r.snap.paused);                            // blackout is separate from pause
    r.act(ActionId::VisualBlackout); r.step(0);            // pressing again does NOT restore
    CHECK(r.snap.visualBlackout);

    r.act(ActionId::LightingBlackout); r.step(0);
    CHECK(r.snap.visualBlackout);
    CHECK(r.snap.lightingBlackout);
    CHECK_FALSE(r.snap.emergencyBlackout);

    r.act(ActionId::RestoreBlackout); r.step(0);
    CHECK_FALSE(r.snap.visualBlackout);
    CHECK_FALSE(r.snap.lightingBlackout);
    CHECK(r.scene() == scene);
    CHECK(r.snap.output(kPrimaryOutput)->params == params);
    CHECK(r.count(EventKind::BlackoutChanged) == 3);
}

TEST_CASE("REQ-D08: emergency blackout wins over every cue and sets both latches") {
    auto d = mkDoc(3);
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 1.0), mkCue(2, 2, 1.0), mkCue(3, 3, 1.0)};
    Rig r(d);
    r.act(ActionId::EmergencyBlackout); r.step(0);
    CHECK(r.snap.visualBlackout);
    CHECK(r.snap.lightingBlackout);
    CHECK(r.snap.emergencyBlackout);
    r.run(secToNs(5));                                     // cues keep running underneath, blackout still wins
    CHECK(r.snap.visualBlackout);
    CHECK(r.snap.lightingBlackout);
    CHECK(r.snap.cueIndex == 2);
    r.act(ActionId::RestoreBlackout); r.step(0);
    CHECK_FALSE(r.snap.emergencyBlackout);
    CHECK_FALSE(r.snap.visualBlackout);
    CHECK_FALSE(r.snap.lightingBlackout);
}

TEST_CASE("REQ-D08: pause and blackout do not interfere") {
    Rig r(mkDoc(4));
    r.act(ActionId::Pause); r.step(0);
    r.act(ActionId::EmergencyBlackout); r.step(0);
    r.act(ActionId::Resume); r.step(0);
    CHECK(r.snap.emergencyBlackout);
    CHECK_FALSE(r.snap.paused);
    r.act(ActionId::Pause); r.step(0);
    r.act(ActionId::RestoreBlackout); r.step(0);
    CHECK(r.snap.paused);
    CHECK_FALSE(r.snap.emergencyBlackout);
}

TEST_CASE("REQ-D08: blackout applies at submit, before the next tick, and survives a document load") {
    Rig r(mkDoc(4));
    r.act(ActionId::VisualBlackout);
    CHECK(r.eng.stats().criticalApplied == 1);
    r.step(0);
    CHECK(r.snap.visualBlackout);
    REQUIRE(r.eng.loadDocument(mkDoc(2)).ok());
    r.step(0);
    CHECK(r.snap.visualBlackout);                          // loading a show never un-blacks
}

TEST_CASE("REQ-D08: quantised trigger on a blackout is ignored and executes immediately") {
    Rig r(mkDoc(2));
    auto res = r.act(ActionId::EmergencyBlackout, 0, kAllOutputs, TriggerKind::NextBar);
    CHECK(res.accepted);
    r.step(0);
    CHECK(r.snap.emergencyBlackout);
}

TEST_CASE("REQ-S12: strobe scenes are rejected and strobe content is stripped when strobing is disabled") {
    auto d = mkDoc(4);
    d.scenes[1].strobe = true;                             // scene 2, pad slot 1
    Rig r(d);
    CHECK(r.act(ActionId::SceneSelect, 1).accepted);       // allowed while strobing is enabled
    r.step(0);
    CHECK(r.scene() == 2);
    CHECK_FALSE(r.snap.output(kPrimaryOutput)->strobeStripped);

    auto dis = r.act(ActionId::DisableStrobing);
    CHECK(dis.accepted);
    r.step(0);
    CHECK(r.snap.strobingDisabled);
    CHECK_FALSE(r.snap.lightingStrobeAllowed);
    CHECK(r.scene() != 2);                                 // live strobe scene was replaced immediately
    CHECK_FALSE(r.snap.output(kPrimaryOutput)->strobeStripped);

    auto rej = r.act(ActionId::SceneSelect, 1);
    CHECK_FALSE(rej.accepted);
    CHECK(rej.reason == "strobing disabled");
    CHECK(r.act(ActionId::SceneSelect, 2).accepted);       // non-strobe pad still fine
    auto nx = r.act(ActionId::SceneSelect, 0, kAllOutputs, TriggerKind::Immediate, 0, 0, 2);
    CHECK_FALSE(nx.accepted);                              // absolute id of the strobe scene is also refused
    CHECK(nx.reason == "strobing disabled");

    CHECK(r.act(ActionId::EnableStrobing).accepted);
    r.step(0);
    CHECK_FALSE(r.snap.strobingDisabled);
    CHECK(r.snap.lightingStrobeAllowed);
}

TEST_CASE("REQ-S12: strobe lock keeps a strobe-only show visible but marks it stripped") {
    auto d = mkDoc(1);
    d.scenes[0].strobe = true;
    Rig r(d);
    r.act(ActionId::DisableStrobing); r.step(0);
    CHECK(r.snap.output(kPrimaryOutput)->hasScene);
    CHECK(r.snap.output(kPrimaryOutput)->strobeStripped);  // no safe scene exists: renderer must suppress flashes
}

TEST_CASE("REQ-S12: strobe effects and cues are rejected or skipped while locked") {
    auto d = mkDoc(3);
    d.scenes[1].strobe = true;
    d.mode = ShowMode::Programmed;
    d.cues = {mkCue(1, 1, 1.0), mkCue(2, 2, 1.0), mkCue(3, 3, 1.0)};
    d.strobingDisabled = true;
    Rig r(d);
    CHECK(r.snap.strobingDisabled);
    r.run(secToNs(5));
    CHECK(r.count(EventKind::CueSkipped) >= 1);
    CHECK(r.snap.cueIndex == 2);                           // went 0 -> 2, never entered the strobe cue
    for (auto& e : r.all) if (e.kind == EventKind::CueFired) CHECK(e.cueIndex != 1);
    auto g = r.act(ActionId::GoToCue, 1);
    CHECK_FALSE(g.accepted);
    CHECK(g.reason == "strobing disabled");
    CHECK(r.act(ActionId::EffectBloomPulse, 0.0).accepted);          // turning the effect off is fine
    auto b = r.act(ActionId::EffectBloomPulse, 0.8);
    CHECK_FALSE(b.accepted);
    CHECK(b.reason == "strobing disabled");
    CHECK(r.act(ActionId::EffectPrismMirror, 0.8).accepted);         // non-flashing effect unaffected
}

TEST_CASE("REQ-S12: an enabled bloom amount is clamped in snapshots once strobing is disabled") {
    Rig r(mkDoc(2));
    r.actNow(ActionId::EffectBloomPulse, 0.9);
    CHECK(r.snap.output(kPrimaryOutput)->params.effectAmount[1] == doctest::Approx(0.9f));
    r.actNow(ActionId::DisableStrobing);
    CHECK(r.snap.output(kPrimaryOutput)->params.effectAmount[1] == doctest::Approx(0.f));
    CHECK(r.snap.output(kPrimaryOutput)->strobeStripped);
    r.actNow(ActionId::EnableStrobing);
    CHECK(r.snap.output(kPrimaryOutput)->params.effectAmount[1] == doctest::Approx(0.9f));
}

TEST_CASE("REQ-S12: loading a document cannot re-enable strobing") {
    Rig r(mkDoc(2));
    r.actNow(ActionId::DisableStrobing);
    auto d = mkDoc(3);
    d.strobingDisabled = false;
    REQUIRE(r.eng.loadDocument(d).ok());
    r.step(0);
    CHECK(r.snap.strobingDisabled);
    CHECK(r.eng.document().strobingDisabled);
}

TEST_CASE("IF-02: invalid actions are rejected with a reason and never change state") {
    Rig r(mkDoc(3));
    const auto rev = r.snap.revision;
    struct Case { ActionId a; double v; OutputId target; const char* reason; };
    const Case cases[] = {
        {ActionId::StartRecording, 0, kAllOutputs, "action not handled by show engine"},
        {ActionId::None, 0, kAllOutputs, "action not handled by show engine"},
        {ActionId::SceneSelect, 9, kAllOutputs, "scene slot out of range"},
        {ActionId::SceneSelect, 5, kAllOutputs, "no scene in that slot"},
        {ActionId::SceneSelect, 1.5, kAllOutputs, "scene slot out of range"},
        {ActionId::SceneSelect, 0, 77, "unknown output"},
        {ActionId::Brightness, 1.5, kAllOutputs, "value out of range"},
        {ActionId::Brightness, std::nan(""), kAllOutputs, "value out of range"},
        {ActionId::Speed, -0.1, kAllOutputs, "value out of range"},
        {ActionId::SetBpm, 5, kAllOutputs, "bpm out of range"},
        {ActionId::SetBpm, 900, kAllOutputs, "bpm out of range"},
        {ActionId::SetKey, 24, kAllOutputs, "key out of range"},
        {ActionId::SetMode, 3, kAllOutputs, "invalid mode"},
        {ActionId::Undo, 0, kAllOutputs, "nothing to undo"},
        {ActionId::Redo, 0, kAllOutputs, "nothing to redo"},
        {ActionId::SnapshotRecallA, 0, kAllOutputs, "snapshot slot is empty"},
        {ActionId::SelectOutput, 42, kAllOutputs, "unknown output"},
        {ActionId::DisarmLighting, 0, kAllOutputs, "action not handled by show engine"},
    };
    for (const auto& c : cases) {
        auto res = r.act(c.a, c.v, c.target);
        CHECK_MESSAGE(!res.accepted, "action " << id(c.a));
        CHECK_MESSAGE(res.reason == c.reason, "action " << id(c.a) << " got '" << res.reason << "'");
    }
    // quantised trigger without a tempo
    auto q = r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::NextBar);
    CHECK_FALSE(q.accepted);
    CHECK(q.reason == "no tempo available for quantised trigger");
    auto neg = r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::AtTime, -5);
    CHECK_FALSE(neg.accepted);
    CHECK(r.eng.stats().queued == 0);
    r.step(0);
    CHECK(r.snap.revision == rev);                         // nothing changed
    CHECK(r.eng.stats().rejected >= 20);
}

TEST_CASE("IF-02: the queue is bounded at 256 and safety-critical actions bypass it") {
    Rig r(mkDoc(4));
    for (std::size_t i = 0; i < Limits::kQueueCapacity; ++i) CHECK(r.act(ActionId::NextScene).accepted);
    auto over = r.act(ActionId::NextScene);
    CHECK_FALSE(over.accepted);
    CHECK(over.reason == "action queue full");
    CHECK(r.eng.stats().queueRejected == 1);
    EditCommand e; e.kind = EditKind::SetPoolMembership; e.sceneId = 1; e.inPool = false;
    CHECK_FALSE(r.eng.submitEdit(e).accepted);             // edits share the same bound
    // critical ids still go through with the queue full
    CHECK(r.act(ActionId::EmergencyBlackout).accepted);
    CHECK(r.act(ActionId::DisableStrobing).accepted);
    CHECK(r.act(ActionId::VisualBlackout).accepted);
    CHECK(r.act(ActionId::LightingBlackout).accepted);
    CHECK(r.act(ActionId::RestoreBlackout).accepted);
    CHECK(r.eng.stats().queued == Limits::kQueueCapacity);
    r.step(0);
    CHECK(r.snap.strobingDisabled);
    CHECK(r.eng.stats().queued == 0);
    CHECK(r.act(ActionId::NextScene).accepted);            // drained: room again
}

TEST_CASE("IF-02: pending quantised actions count toward the queue bound") {
    Rig r(mkDoc(4));
    r.act(ActionId::SetBpm, 120.0); r.step(0);
    for (std::size_t i = 0; i < Limits::kQueueCapacity; ++i) CHECK(r.act(ActionId::NextScene, 0, kAllOutputs, TriggerKind::NextBar).accepted);
    r.step(msToNs(10));                                    // moved from inbox to pending, still bounded
    CHECK(r.eng.stats().pending == Limits::kQueueCapacity);
    CHECK_FALSE(r.act(ActionId::NextScene).accepted);
    CHECK(r.act(ActionId::EmergencyBlackout).accepted);
}

TEST_CASE("IF-02: stale revision expectations are rejected") {
    Rig r(mkDoc(4));
    const Revision rev = r.snap.revision;
    CHECK(r.act(ActionId::Brightness, 0.5, kAllOutputs, TriggerKind::Immediate, 0, rev).accepted);
    r.step(0);
    CHECK(r.snap.revision > rev);
    auto stale = r.act(ActionId::Brightness, 0.6, kAllOutputs, TriggerKind::Immediate, 0, rev);
    CHECK_FALSE(stale.accepted);
    CHECK(stale.reason == "stale revision");
    CHECK(r.act(ActionId::Brightness, 0.6, kAllOutputs, TriggerKind::Immediate, 0, r.snap.revision).accepted);
    CHECK(r.act(ActionId::Brightness, 0.7).accepted);      // 0 = do not check
    // safety-critical actions are never refused for staleness
    CHECK(r.act(ActionId::EmergencyBlackout, 0, kAllOutputs, TriggerKind::Immediate, 0, rev).accepted);
    EditCommand e; e.kind = EditKind::SetPoolMembership; e.sceneId = 1; e.inPool = false; e.expectedRevision = rev;
    CHECK_FALSE(r.eng.submitEdit(e).accepted);
}

TEST_CASE("IF-05: revisions are monotonic and only bump on discrete changes") {
    Rig r(mkDoc(4), [] { auto t = fastTun(); t.autoIntervalSec = 1000; return t; }());
    Revision last = r.snap.revision;
    for (int i = 0; i < 50; ++i) { r.step(msToNs(100)); CHECK(r.snap.revision >= last); last = r.snap.revision; }
    const Revision steady = r.snap.revision;
    r.run(secToNs(5));
    CHECK(r.snap.revision == steady);                      // idle time does not churn the revision
    r.actNow(ActionId::NextScene);
    CHECK(r.snap.revision > steady);
    CHECK(r.snap.lite().revision == r.snap.revision);
}

TEST_CASE("IF-11: a failed document swap leaves the old state active") {
    Rig r(mkDoc(4));
    r.actNow(ActionId::SceneSelect, 2);
    const auto scene = r.scene();
    const auto rev = r.snap.revision;
    auto bad = mkDoc(3);
    bad.scenes.push_back(bad.scenes[0]);                   // duplicate scene id
    auto res = r.eng.loadDocument(bad);
    CHECK_FALSE(res.ok());
    CHECK(res.error().category == ErrorCategory::InvalidArgument);
    r.step(msToNs(100));
    CHECK(r.scene() == scene);
    CHECK(r.snap.revision == rev);
    CHECK(r.eng.document().scenes.size() == 4);

    auto bad2 = mkDoc(3); bad2.scenes[0].params.brightness = 2.f;
    CHECK_FALSE(r.eng.loadDocument(bad2).ok());
    auto bad3 = mkDoc(3); bad3.outputs.clear();
    CHECK_FALSE(r.eng.loadDocument(bad3).ok());
    auto bad4 = mkDoc(3); bad4.schemaMajor = 99;
    CHECK(r.eng.loadDocument(bad4).error().category == ErrorCategory::SchemaUnsupported);
    auto bad5 = mkDoc(3); bad5.outputs.push_back({}); bad5.outputs.back().id = 2; bad5.outputs.back().kind = RouteKind::Duplicate; bad5.outputs.back().duplicateOf = 2;
    CHECK_FALSE(r.eng.loadDocument(bad5).ok());
    r.run(msToNs(100));
    CHECK(r.eng.document().scenes.size() == 4);
}

TEST_CASE("IF-11: a valid document swaps at the next tick boundary and resets show state") {
    Rig r(mkDoc(4));
    r.actNow(ActionId::SceneSelect, 2);
    REQUIRE(r.snap.manualOverride);
    REQUIRE(r.eng.loadDocument(mkDoc(6, 5)).ok());
    CHECK(r.eng.document().scenes.size() == 4);            // staged, not yet live
    r.step(0);
    CHECK(r.eng.document().scenes.size() == 6);
    CHECK_FALSE(r.snap.manualOverride);
    CHECK(r.snap.seed == 5);
    CHECK(r.snap.undoDepth == 0);
    CHECK(r.count(EventKind::DocumentLoaded) == 2);
}
