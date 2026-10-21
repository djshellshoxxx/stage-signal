#include "doctest/doctest.h"
#include "ss/diag/journal.hpp"
#include "ss/diag/sampler.hpp"

using namespace ss;
using namespace ss::diag;

static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

TEST_CASE("REQ-X05: journal events carry monotonic and wall-clock references") {
    FakeClock clock(5 * kNsPerSec, 1'700'000'000LL * kNsPerSec);
    EventJournal j(clock);
    j.append(EventKind::Scene, "to Neon Grid", 4);
    clock.advance(msToNs(1500));
    j.append(EventKind::Mode, "Auto -> Hybrid");
    const auto ev = j.snapshot();
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].monoNs == 5 * kNsPerSec);
    CHECK(ev[0].wallUnixMs == 1'700'000'000'000LL);
    CHECK(ev[0].id == 4);
    CHECK(ev[1].monoNs == 5 * kNsPerSec + msToNs(1500));
    CHECK(ev[1].wallUnixMs == 1'700'000'001'500LL);
    CHECK(ev[0].seq == 1);
    CHECK(ev[1].seq == 2);
    CHECK(std::string(kindName(ev[0].kind)) == "scene");
}

TEST_CASE("REQ-X05: journal is a bounded ring that counts what it overwrites") {
    FakeClock clock;
    EventJournal j(clock, 16);
    CHECK(j.capacity() == 16);
    for (int i = 0; i < 50; ++i) { clock.advance(msToNs(10)); j.append(EventKind::Note, "n" + std::to_string(i)); }
    CHECK(j.size() == 16);
    CHECK(j.overwritten() == 34);
    CHECK(j.totalAppended() == 50);
    const auto ev = j.snapshot();
    REQUIRE(ev.size() == 16);
    CHECK(ev.front().detail == "n34");
    CHECK(ev.back().detail == "n49");
    for (std::size_t i = 1; i < ev.size(); ++i) { CHECK(ev[i].seq == ev[i - 1].seq + 1); CHECK(ev[i].monoNs >= ev[i - 1].monoNs); }
    // capacity is clamped
    EventJournal tiny(clock, 1), huge(clock, 1u << 30);
    CHECK(tiny.capacity() == kJournalMinCapacity);
    CHECK(huge.capacity() == kJournalMaxCapacity);
    j.clear();
    CHECK(j.size() == 0);
}

TEST_CASE("REQ-B03: journal text is sanitised on the way in") {
    FakeClock clock;
    EventJournal j(clock);
    j.append(EventKind::Broadcast, "start rtmps://bob:pw@live.example.com/app/abcd1234efgh5678ijkl?token=zzz key=SuperSecret99 /home/bob/x");
    j.append(EventKind::Error, "line1\nline2\ttab\x01" "end");
    j.append(EventKind::Note, std::string(5000, 'x'));
    j.append(EventKind::Note, "bad\xff\xfe" "bytes");
    const auto ev = j.snapshot();
    CHECK_FALSE(has(ev[0].detail, "bob:pw"));
    CHECK_FALSE(has(ev[0].detail, "abcd1234efgh5678ijkl"));
    CHECK_FALSE(has(ev[0].detail, "zzz"));
    CHECK_FALSE(has(ev[0].detail, "SuperSecret99"));
    CHECK_FALSE(has(ev[0].detail, "/home/bob"));
    CHECK(ev[1].detail == "line1 line2 tab end");
    CHECK(ev[2].detail.size() == kJournalDetailMaxBytes);
    CHECK(ev[3].detail == "bad??bytes");
}

TEST_CASE("REQ-X05: marker API") {
    FakeClock clock;
    EventJournal j(clock);
    j.addMarker("Song A");
    clock.advance(secToNs(60));
    j.addMarker("");
    j.addMarker("  <b>Encore</b>\n  ");
    j.append(EventKind::Scene, "x");
    j.addMarker(std::string(500, 'm'));
    const auto m = j.markers();
    REQUIRE(m.size() == 4);
    CHECK(m[0].detail == "Song A");
    CHECK(m[1].detail == "Marker 2");
    CHECK(m[2].detail == "<b>Encore</b>");   // stored raw but sanitised; escaping is the renderer's job
    CHECK(m[3].detail.size() == 120);
    CHECK(j.markerCount() == 4);
    CHECK(m[1].monoNs - m[0].monoNs == secToNs(60));
}

TEST_CASE("REQ-X05: journal converts to the show document's JournalEntry list, newest entries kept") {
    FakeClock clock;
    EventJournal j(clock);
    for (int i = 0; i < 10; ++i) { clock.advance(msToNs(1)); j.append(i % 2 ? EventKind::Recording : EventKind::Broadcast, "e" + std::to_string(i)); }
    const auto m = j.toModelEntries(4);
    REQUIRE(m.size() == 4);
    CHECK(m.front().detail == "e6");
    CHECK(m.front().kind == "cast");
    CHECK(m[1].kind == "rec");
    EventKind k;
    CHECK(parseKindName("gap", k));
    CHECK(k == EventKind::Discontinuity);
    CHECK_FALSE(parseKindName("nope", k));
}

TEST_CASE("REQ-X05: tempo and key snapshots are sampled at no more than 1 Hz") {
    TempoKeySampler s;
    FeatureSnapshot snap;
    snap.tempo.bpm = 128.0; snap.tempo.source = ValueSource::Estimated; snap.tempo.confidence = 0.9f;
    snap.key.tonic = 9; snap.key.minor = true; snap.key.source = ValueSource::Estimated;
    snap.energyArc = 0.4f; snap.arcPhase = 1;
    int emitted = 0;
    Ns last = -1;
    for (int i = 0; i < 1000; ++i) {                    // 10 s of snapshots at 100 Hz
        FeatureSample out;
        const Ns t = msToNs(10 * i);
        if (s.offer(t, snap, 3, out)) {
            ++emitted;
            if (last >= 0) CHECK(out.monoNs - last >= kNsPerSec);
            last = out.monoNs;
            CHECK(out.bpm == doctest::Approx(128.0));
            CHECK(out.keyTonic == 9);
            CHECK(out.keyMinor);
            CHECK(out.sceneId == 3);
            CHECK(out.arcPhase == 1);
        }
    }
    CHECK(emitted == 10);
}

TEST_CASE("REQ-X05: unknown tempo/key are stored as unknown, never as zero BPM or key C") {
    TempoKeySampler s;
    FeatureSnapshot snap;   // defaults: Unknown sources
    snap.energyArc = 7.f;   // out of range is clamped
    FeatureSample out;
    REQUIRE(s.offer(0, snap, 0, out));
    CHECK(out.bpm == 0.0);
    CHECK(out.keyTonic == -1);
    CHECK(out.energyArc == 1.f);
    // time going backwards is never emitted, and does not reset the cadence
    CHECK_FALSE(s.offer(-5 * kNsPerSec, snap, 0, out));
    CHECK_FALSE(s.offer(kNsPerSec - 1, snap, 0, out));
    CHECK(s.offer(kNsPerSec, snap, 0, out));
}

TEST_CASE("REQ-X05: feature history is bounded") {
    FeatureHistory h(8);
    FeatureSnapshot snap;
    for (int i = 0; i < 30; ++i) h.offer(secToNs(i), snap, static_cast<std::uint32_t>(i));
    CHECK(h.size() == 8);
    CHECK(h.overwritten() == 22);
    const auto s = h.samples();
    REQUIRE(s.size() == 8);
    CHECK(s.front().sceneId == 22);
    CHECK(s.back().sceneId == 29);
}
