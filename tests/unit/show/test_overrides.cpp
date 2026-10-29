#include <cmath>
#include "rig.hpp"
using namespace sst;

namespace {
Tunables quiet() { auto t = fastTun(); t.autoIntervalSec = 100000.0; return t; }
}

TEST_CASE("REQ-A06: the document carries the active tempo override (manual and tap)") {
    Rig r(mkDoc(3), quiet());
    CHECK_FALSE(r.eng.document().overrides.tempoActive);
    r.actNow(ActionId::SetBpm, 140.0);
    auto o = r.eng.document().overrides;
    CHECK(o.tempoActive); CHECK(o.bpm == doctest::Approx(140.0)); CHECK_FALSE(o.tempoTap);
    r.actNow(ActionId::ReleaseBpmOverride);
    CHECK_FALSE(r.eng.document().overrides.tempoActive);

    auto tapAt = [&](Ns ts) { r.act(ActionId::TapTempo, 0, kAllOutputs, TriggerKind::Immediate, ts); r.step(0); };
    tapAt(secToNs(10)); tapAt(secToNs(10) + msToNs(600)); tapAt(secToNs(10) + msToNs(1200));
    o = r.eng.document().overrides;
    CHECK(o.tempoActive); CHECK(o.bpm == doctest::Approx(100.0).epsilon(0.001)); CHECK(o.tempoTap);
}

TEST_CASE("REQ-A08: save and load preserves the manual key and tempo override on a fresh engine") {
    Rig a(mkDoc(3), quiet());
    a.actNow(ActionId::SetBpm, 133.0);
    a.actNow(ActionId::SetKey, 9 + 12);                    // A minor
    const model::ShowDocument saved = a.eng.document();

    Rig b(saved, quiet());                                  // a different engine, as after a restart
    b.f.tempo.bpm = 90.0; b.f.tempo.source = ValueSource::Estimated; b.f.tempo.confidence = 0.9f;
    b.f.key.tonic = 2; b.f.key.source = ValueSource::Estimated; b.f.key.confidence = 0.9f;
    b.step(msToNs(100));
    CHECK(b.snap.tempoOverride);
    CHECK(b.snap.tempo.bpm == doctest::Approx(133.0));      // the detector does not win
    CHECK(b.snap.tempo.source == ValueSource::Manual);
    CHECK(b.snap.keyOverride);
    CHECK(b.snap.key.tonic == 9); CHECK(b.snap.key.minor);
    CHECK(b.snap.key.source == ValueSource::Manual);

    b.actNow(ActionId::ReleaseBpmOverride);                 // still releasable after a load
    CHECK_FALSE(b.snap.tempoOverride);
    CHECK(b.snap.tempo.bpm == doctest::Approx(90.0));
}

TEST_CASE("REQ-A06: a tap override keeps its Tap label across save and load") {
    Rig a(mkDoc(3), quiet());
    auto tapAt = [&](Ns ts) { a.act(ActionId::TapTempo, 0, kAllOutputs, TriggerKind::Immediate, ts); a.step(0); };
    tapAt(secToNs(10)); tapAt(secToNs(10) + msToNs(500)); tapAt(secToNs(11));
    Rig b(a.eng.document(), quiet());
    b.step(msToNs(100));
    CHECK(b.snap.tempoOverride);
    CHECK(b.snap.tempo.source == ValueSource::Tap);
    CHECK(b.snap.tempo.bpm == doctest::Approx(120.0).epsilon(0.001));
}

TEST_CASE("REQ-A06: loading a show without an override keeps the operator's running override") {
    Rig r(mkDoc(3), quiet());
    r.actNow(ActionId::SetBpm, 150.0);
    REQUIRE(r.eng.loadDocument(mkDoc(4)).ok());
    r.step(msToNs(100));
    CHECK(r.snap.tempoOverride);                            // documented engine rule: a load never drops the operator's override
    CHECK(r.snap.tempo.bpm == doctest::Approx(150.0));
}

TEST_CASE("REQ-A06: the engine rejects a document with an invalid override and keeps the old show") {
    Rig r(mkDoc(3), quiet());
    auto bad = mkDoc(3);
    bad.overrides.tempoActive = true; bad.overrides.bpm = 1000.0;
    CHECK_FALSE(r.eng.loadDocument(bad).ok());
    bad.overrides.bpm = 120.0; bad.overrides.keyActive = true; bad.overrides.tonic = 12;
    CHECK_FALSE(r.eng.loadDocument(bad).ok());
    bad.overrides.tonic = 3; bad.overrides.keyMinor = true;
    CHECK(ShowEngine::validateDocument(bad).ok());
    bad.overrides.bpm = std::nan("");
    CHECK_FALSE(ShowEngine::validateDocument(bad).ok());
    r.step(msToNs(100));
    CHECK_FALSE(r.snap.tempoOverride);                      // nothing was applied
}
