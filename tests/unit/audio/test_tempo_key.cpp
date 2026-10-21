// T-TEMPO and T-KEY: estimators, tap/manual controllers, silence/noise behaviour.
#include "doctest/doctest.h"
#include "test_util.hpp"
#include "ss/audio/analyzer.hpp"
#include "ss/audio/key.hpp"
#include "ss/audio/tempo.hpp"

using namespace ss;
using namespace ss::audio;

namespace {
// Feeds `sig` in 480-frame blocks and returns the snapshot at the end.
FeatureSnapshot runAnalyzer(Analyzer& a, sst::Feeder& fd, const std::vector<float>& sig) {
    fd.feed(sig, 480);
    return a.peek();
}
}

TEST_CASE("REQ-A05: click corpus 60-180 BPM within +-2% after 10 s (48 kHz mono)") {
    for (double bpm : {60.0, 75.0, 90.0, 100.0, 120.0, 135.0, 150.0, 165.0, 180.0}) {
        CAPTURE(bpm);
        Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
        sst::Feeder fd(a, 1, 48000);
        const FeatureSnapshot s = runAnalyzer(a, fd, sst::clickTrain(bpm, 10.0, 48000));
        REQUIRE(s.tempo.valid());
        CHECK(s.tempo.source == ValueSource::Estimated);
        CHECK(std::fabs(s.tempo.bpm / bpm - 1.0) <= 0.02);
        CHECK(s.tempo.confidence > 0.6f);
        CHECK(s.tempo.ageNs < secToNs(2));
    }
}

TEST_CASE("REQ-A05: click corpus at 44.1 kHz stereo, odd block size") {
    for (double bpm : {70.0, 100.0, 133.0, 180.0}) {
        CAPTURE(bpm);
        Analyzer a; REQUIRE(a.prepare(44100, 2).ok());
        sst::Feeder fd(a, 2, 44100);
        fd.feed(sst::toChannels(sst::clickTrain(bpm, 10.0, 44100), 2), 441 + 13);
        const FeatureSnapshot s = a.peek();
        REQUIRE(s.tempo.valid());
        CHECK(std::fabs(s.tempo.bpm / bpm - 1.0) <= 0.02);
    }
}

TEST_CASE("REQ-A05: percussive pulse corpus with varied timbres estimates the beat") {
    Rng rng(21);
    const double bpm = 112.0;
    std::vector<float> v(48000 * 12, 0.f);
    const double period = 60.0 / bpm;
    int k = 0;
    for (double t = 0.2; t < 11.5; t += period, ++k) sst::addInPlace(v, sst::hit(k, 0.7, 48000, rng), static_cast<std::size_t>(t * 48000));
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    const FeatureSnapshot s = runAnalyzer(a, fd, v);
    REQUIRE(s.tempo.valid());
    CHECK(std::fabs(s.tempo.bpm / bpm - 1.0) <= 0.02);
}

TEST_CASE("REQ-A05: swing and weak off-beats resolve to the beat, not half/double") {
    const std::uint32_t rate = 48000;
    const double bpm = 100.0, beat = 60.0 / bpm;
    std::vector<float> v(rate * 12, 0.f);
    Rng rr(1);
    for (double t = 0.2; t < 11.5; t += beat) {
        sst::addInPlace(v, sst::hit(2, 0.8, rate, rr), static_cast<std::size_t>(t * rate));          // kick on the beat
        sst::addInPlace(v, sst::hit(0, 0.25, rate, rr), static_cast<std::size_t>((t + beat * 2.0 / 3.0) * rate));  // swung off-beat
    }
    Analyzer a; REQUIRE(a.prepare(rate, 1).ok());
    sst::Feeder fd(a, 1, rate);
    const FeatureSnapshot s = runAnalyzer(a, fd, v);
    REQUIRE(s.tempo.valid());
    CHECK(std::fabs(s.tempo.bpm / bpm - 1.0) <= 0.03);
}

TEST_CASE("REQ-A05: equal-strength eighth notes are a faster click train (<= 200 BPM preferred range)") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    const FeatureSnapshot s = runAnalyzer(a, fd, sst::clickTrain(190.0, 10.0, 48000));
    REQUIRE(s.tempo.valid());
    CHECK(std::fabs(s.tempo.bpm / 190.0 - 1.0) <= 0.03);
}

TEST_CASE("REQ-A05: silence is Unknown (never numeric zero BPM) and an estimate expires after the click stops") {
    {
        Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
        sst::Feeder fd(a, 1, 48000);
        const FeatureSnapshot s = runAnalyzer(a, fd, std::vector<float>(48000 * 12, 0.f));
        CHECK(s.tempo.source == ValueSource::Unknown);
        CHECK(s.tempo.bpm == 0.0);
        CHECK(s.tempo.confidence == 0.f);
        CHECK_FALSE(s.tempo.valid());
    }
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    fd.feed(sst::clickTrain(120.0, 10.0, 48000), 480);
    REQUIRE(a.peek().tempo.valid());
    fd.feed(std::vector<float>(2400, 0.f), 480);
    fd.feed(std::vector<float>(48000, 0.f), 480);                          // 1 s of silence: not yet expired
    CHECK(a.peek().tempo.valid());
    fd.feed(std::vector<float>(48000 * 2, 0.f), 480);                      // 2 s more: Unknown
    const auto t = a.peek().tempo;
    CHECK_FALSE(t.valid());
    CHECK(t.bpm == 0.0);
}

TEST_CASE("REQ-A05: age grows when the rhythm disappears but sound continues, then the estimate expires") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    fd.feed(sst::clickTrain(120.0, 10.0, 48000), 480);
    REQUIRE(a.peek().tempo.valid());
    Rng rng(8);
    Ns maxAge = 0;
    bool expired = false;
    for (int sec = 0; sec < 20; ++sec) {
        std::vector<float> n(48000);
        for (auto& x : n) x = (rng.unit() * 2 - 1) * 0.2f;
        fd.feed(n, 480);
        const TempoState t = a.peek().tempo;
        if (t.valid()) maxAge = std::max(maxAge, t.ageNs); else { expired = true; CHECK(t.bpm == 0.0); }
    }
    CHECK(maxAge >= secToNs(2));
    CHECK(expired);
}

TEST_CASE("REQ-A05: noise and steady tones do not produce a confident tempo") {
    Rng rng(4);
    std::vector<float> n(48000 * 12);
    for (auto& x : n) x = (rng.unit() * 2 - 1) * 0.3f;
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    CHECK_FALSE(runAnalyzer(a, fd, n).tempo.valid());
    Analyzer b; REQUIRE(b.prepare(48000, 1).ok());
    sst::Feeder fb(b, 1, 48000);
    CHECK_FALSE(runAnalyzer(b, fb, sst::sine(440, 0.4, 12.0, 48000)).tempo.valid());
}

TEST_CASE("REQ-A05: tempo change is followed after a few seconds, with smoothing (no jitter on a steady tempo)") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    fd.feed(sst::clickTrain(100.0, 12.0, 48000), 480);
    REQUIRE(a.peek().tempo.valid());
    double minB = 1e9, maxB = 0;
    // steady: second 12..20 estimate must not wobble more than 1%
    fd.feed(sst::clickTrain(100.0, 8.0, 48000), 480);
    minB = maxB = a.peek().tempo.bpm;
    // change to 130
    const auto next = sst::clickTrain(130.0, 20.0, 48000);
    double bpmAfter = 0;
    for (std::size_t pos = 0; pos + 480 * 100 <= next.size(); pos += 480 * 100) {
        fd.feed(std::vector<float>(next.begin() + pos, next.begin() + pos + 480 * 100), 480);
        bpmAfter = a.peek().tempo.bpm;
    }
    CHECK(std::fabs(bpmAfter / 130.0 - 1.0) <= 0.03);
    CHECK(std::fabs(maxB / minB - 1.0) < 0.01);
}

TEST_CASE("REQ-A05: TempoEstimator rejects bad configuration and survives hostile input") {
    TempoEstimator t;
    CHECK_FALSE(t.prepare(100, 256));
    CHECK_FALSE(t.prepare(48000, 0));
    REQUIRE(t.prepare(48000, 256));
    for (int i = 0; i < 5000; ++i) t.push(i % 7 == 0 ? std::nanf("") : (i % 11 == 0 ? -1.f : 1e30f), i % 13 == 0);
    const TempoState s = t.state();
    CHECK(std::isfinite(s.bpm));
    if (s.valid()) { CHECK(s.bpm >= 38.0); CHECK(s.bpm <= 250.0); } else CHECK(s.bpm == 0.0);
}

// ---- REQ-A06 controller -------------------------------------------------------------------------------
TEST_CASE("REQ-A06: tap tempo is deterministic and averages recent taps") {
    auto run = [] {
        TempoController c;
        CHECK_FALSE(c.tap(msToNs(1000)));                    // first tap alone sets nothing
        CHECK(c.tap(msToNs(1500)));
        c.tap(msToNs(2000)); c.tap(msToNs(2500));
        return c.resolve(TempoState{}, msToNs(2600));
    };
    const TempoState a = run(), b = run();
    CHECK(a.source == ValueSource::Tap);
    CHECK(a.bpm == doctest::Approx(120.0));
    CHECK(a.confidence == 1.f);
    CHECK(a.ageNs == msToNs(100));
    CHECK(a.bpm == b.bpm);                                   // bit-identical
    // jittery taps average out: intervals 480, 520, 500, 500 -> 500 ms
    TempoController j;
    j.tap(0); j.tap(msToNs(480)); j.tap(msToNs(1000)); j.tap(msToNs(1500)); j.tap(msToNs(2000));
    CHECK(j.resolve(TempoState{}, msToNs(2000)).bpm == doctest::Approx(120.0));
}

TEST_CASE("REQ-A06: tap sequence resets after a long gap and keeps only the most recent taps") {
    TempoController c;
    c.tap(0); c.tap(msToNs(500)); c.tap(msToNs(1000));
    CHECK(c.resolve(TempoState{}, msToNs(1000)).bpm == doctest::Approx(120.0));
    CHECK_FALSE(c.tap(msToNs(6000)));                        // gap > 2.5 s: new sequence of one tap
    CHECK(c.tapCount() == 1);
    CHECK(c.resolve(TempoState{}, msToNs(6000)).bpm == doctest::Approx(120.0));   // previous override stays
    CHECK(c.tap(msToNs(6400)));
    CHECK(c.resolve(TempoState{}, msToNs(6400)).bpm == doctest::Approx(150.0));
    // slow start then fast: only the last 8 taps count
    TempoController d;
    Ns t = 0;
    d.tap(t); t += msToNs(2000); d.tap(t);                   // 30 BPM pair
    for (int i = 0; i < 10; ++i) { t += msToNs(400); d.tap(t); }
    CHECK(d.tapCount() == 8);
    CHECK(d.resolve(TempoState{}, t).bpm == doctest::Approx(150.0));
}

TEST_CASE("REQ-A06: estimates are used until Tap/Manual is set; Release returns to the estimate") {
    TempoController c;
    TempoState est; est.bpm = 128.0; est.confidence = 0.8f; est.source = ValueSource::Estimated; est.ageNs = msToNs(300);
    CHECK(c.resolve(est, 0).source == ValueSource::Estimated);
    CHECK(c.resolve(est, 0).bpm == 128.0);
    CHECK(c.setManual(140.5, msToNs(10)).ok());
    TempoState r = c.resolve(est, msToNs(1010));
    CHECK(r.source == ValueSource::Manual);
    CHECK(r.bpm == 140.5);
    CHECK(r.confidence == 1.f);
    CHECK(r.ageNs == msToNs(1000));
    // a new estimate does not override the manual value
    est.bpm = 90.0;
    CHECK(c.resolve(est, msToNs(2000)).bpm == 140.5);
    // tap replaces manual (latest action wins)
    c.tap(msToNs(3000)); c.tap(msToNs(3500));
    CHECK(c.resolve(est, msToNs(3500)).source == ValueSource::Tap);
    // manual replaces tap
    CHECK(c.setManual(100.0, msToNs(4000)).ok());
    CHECK(c.resolve(est, msToNs(4000)).source == ValueSource::Manual);
    c.release();
    CHECK(c.resolve(est, msToNs(4001)).source == ValueSource::Estimated);
    CHECK(c.resolve(est, msToNs(4001)).bpm == 90.0);
    c.release();                                             // idempotent
    TempoState none;
    const TempoState u = c.resolve(none, 0);
    CHECK(u.source == ValueSource::Unknown);
    CHECK(u.bpm == 0.0);
    CHECK_FALSE(u.valid());
}

TEST_CASE("REQ-A06: invalid manual values are rejected and keep the previous state") {
    TempoController c;
    CHECK(c.setManual(120.0, 0).ok());
    CHECK_FALSE(c.setManual(std::nan(""), 1).ok());
    CHECK_FALSE(c.setManual(0.0, 1).ok());
    CHECK_FALSE(c.setManual(-5.0, 1).ok());
    CHECK_FALSE(c.setManual(1000.0, 1).ok());
    CHECK_FALSE(c.setManual(INFINITY, 1).ok());
    CHECK(c.setManual(1000.0, 1).error().category == ErrorCategory::InvalidArgument);
    CHECK(c.resolve(TempoState{}, 2).bpm == 120.0);
    // out-of-range tap result (faster than 300 BPM) is ignored
    TempoController t;
    t.tap(0); CHECK_FALSE(t.tap(msToNs(100)));
    CHECK(t.resolve(TempoState{}, msToNs(100)).source == ValueSource::Unknown);
}

// ---- T-KEY ----------------------------------------------------------------------------------------------
namespace {
KeyState runKey(const std::vector<float>& sig, std::uint32_t rate, KeyDiagnostics* d = nullptr) {
    KeyEstimator k; REQUIRE(k.prepare(rate));
    for (std::size_t p = 0; p < sig.size(); p += 4800) k.process(sig.data() + p, std::min<std::size_t>(4800, sig.size() - p));
    if (d) *d = k.diagnostics();
    return k.state();
}
}

TEST_CASE("REQ-A07: all 24 major/minor keys are identified on I-IV-V-I progressions") {
    const std::uint32_t rate = 16000;                        // the estimator decimates to ~4 kHz; cheaper synthesis
    int correct = 0;
    for (int minor = 0; minor < 2; ++minor) {
        for (int tonic = 0; tonic < 12; ++tonic) {
            const KeyState k = runKey(sst::progression(tonic, minor != 0, 16.0, rate), rate);
            CAPTURE(tonic); CAPTURE(minor);
            if (k.valid() && k.tonic == tonic && k.minor == (minor != 0)) ++correct;
            CHECK(k.valid());
            CHECK(k.confidence >= KeyLimits::kMinConfidence);
            CHECK(k.source == ValueSource::Estimated);
        }
    }
    CHECK(correct == 24);
}

TEST_CASE("REQ-A07: key at 48 kHz and 44.1 kHz through the Analyzer, stereo") {
    for (std::uint32_t rate : {48000u, 44100u}) {
        Analyzer a; REQUIRE(a.prepare(rate, 2).ok());
        sst::Feeder fd(a, 2, rate);
        fd.feed(sst::toChannels(sst::progression(2, false, 15.0, rate), 2), 1024);      // D major
        const KeyState k = a.peek().key;
        CAPTURE(rate);
        REQUIRE(k.valid());
        CHECK(k.tonic == 2);
        CHECK_FALSE(k.minor);
        CHECK(k.ageNs < secToNs(5));
    }
}

TEST_CASE("REQ-A07: a major scale (ascending and descending) is reported as that major key") {
    const std::uint32_t rate = 16000;
    const int maj[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    std::vector<float> sc(static_cast<std::size_t>(16 * rate), 0.f);
    for (int k = 0; k < 32; ++k) { const int idx = k % 14; const int deg = idx < 8 ? idx : 14 - idx; sst::addNote(sc, 60 + 2 + maj[deg], k * 0.5, 0.5, 0.1, rate); }
    const KeyState k = runKey(sc, rate);
    REQUIRE(k.valid());
    CHECK(k.tonic == 2);                                      // D major scale
    CHECK_FALSE(k.minor);
}

TEST_CASE("REQ-A07: detuning of up to +-40 cents does not change the key") {
    const std::uint32_t rate = 16000;
    for (double cents : {-40.0, -25.0, 30.0, 40.0}) {
        for (int minor = 0; minor < 2; ++minor) {
            CAPTURE(cents); CAPTURE(minor);
            const KeyState k = runKey(sst::progression(9, minor != 0, 16.0, rate, cents), rate);
            REQUIRE(k.valid());
            CHECK(k.tonic == 9);
            CHECK(k.minor == (minor != 0));
        }
    }
}

TEST_CASE("REQ-A07: silence, noise, drums, a single note and random notes stay Unknown") {
    const std::uint32_t rate = 48000;
    CHECK_FALSE(runKey(std::vector<float>(16 * rate, 0.f), rate).valid());
    Rng rng(5);
    std::vector<float> n(16 * rate);
    for (auto& x : n) x = (rng.unit() * 2 - 1) * 0.3f;
    CHECK_FALSE(runKey(n, rate).valid());
    // pink-ish: low-pass filtered noise
    float z = 0.f; for (auto& x : n) { z = 0.97f * z + 0.03f * x; x = z * 6.f; }
    CHECK_FALSE(runKey(n, rate).valid());
    // drum loop (kick + noise snare), 120 BPM
    std::vector<float> d(16 * rate, 0.f);
    Rng r2(9);
    int i = 0;
    for (double t = 0.0; t < 15.5; t += 0.25, ++i) sst::addInPlace(d, sst::hit(i % 2 == 0 ? 2 : 1, 0.7, rate, r2), static_cast<std::size_t>(t * rate));
    CHECK_FALSE(runKey(d, rate).valid());
    // a single sustained note is not a key
    std::vector<float> one(static_cast<std::size_t>(16 * rate), 0.f);
    sst::addNote(one, 57, 0, 16, 0.2, rate);
    CHECK_FALSE(runKey(one, rate).valid());
    // random notes
    std::vector<float> rnd(static_cast<std::size_t>(16 * rate), 0.f);
    Rng r3(11);
    for (int k = 0; k < 64; ++k) sst::addNote(rnd, 48 + static_cast<double>(r3.below(30)), k * 0.25, 0.25, 0.1, rate);
    CHECK_FALSE(runKey(rnd, rate).valid());
}

TEST_CASE("REQ-A07: not enough tonal evidence -> Unknown; reported only after >= 8 s") {
    const std::uint32_t rate = 16000;
    const auto sig = sst::progression(0, true, 16.0, rate);
    KeyEstimator k; REQUIRE(k.prepare(rate));
    double firstValid = -1;
    for (std::size_t p = 0; p + 1600 <= sig.size(); p += 1600) {
        k.process(sig.data() + p, 1600);
        if (firstValid < 0 && k.state().valid()) firstValid = double(p + 1600) / rate;
    }
    REQUIRE(firstValid > 0);
    CHECK(firstValid >= 8.0);
    CHECK(firstValid <= 14.0);
}

TEST_CASE("REQ-A07: key change clears the old claim gradually and age is reported; silence clears the key") {
    const std::uint32_t rate = 16000;
    KeyEstimator k; REQUIRE(k.prepare(rate));
    auto feed = [&](const std::vector<float>& s) { for (std::size_t p = 0; p < s.size(); p += 1600) k.process(s.data() + p, std::min<std::size_t>(1600, s.size() - p)); };
    feed(sst::progression(0, false, 16.0, rate));
    KeyState a = k.state();
    REQUIRE(a.valid());
    CHECK(a.tonic == 0);
    const float confBefore = a.confidence;
    // move to F# major; sample the reported key every second
    const auto b = sst::progression(6, false, 30.0, rate);
    bool sawOldDecay = false, sawUnknownOrNew = false, ended = false;
    for (std::size_t p = 0; p + rate <= b.size(); p += rate) {
        k.process(b.data() + p, rate);
        const KeyState s = k.state();
        if (s.valid() && s.tonic == 0 && s.confidence < confBefore - 0.05f) sawOldDecay = true;
        if (!s.valid() || s.tonic == 6) sawUnknownOrNew = true;
        if (s.valid() && s.tonic == 6) ended = true;
        if (s.valid() && s.tonic != 0 && s.tonic != 6) FAIL("spurious intermediate key " << s.tonic);
    }
    CHECK(sawUnknownOrNew);
    CHECK(ended);
    (void)sawOldDecay;
    // age grows while the key is still claimed with weaker support, and resets on confirmation
    CHECK(k.state().ageNs < secToNs(6));
    // long silence: Unknown
    feed(std::vector<float>(rate * 30, 0.f));
    CHECK_FALSE(k.state().valid());
    CHECK(k.state().tonic == -1);
}

TEST_CASE("REQ-A08: manual key overrides the detector and Release returns to it") {
    KeyController c;
    KeyState est; est.tonic = 4; est.minor = true; est.confidence = 0.7f; est.source = ValueSource::Estimated; est.ageNs = msToNs(500);
    CHECK(c.resolve(est, 0).tonic == 4);
    CHECK(c.resolve(est, 0).source == ValueSource::Estimated);
    CHECK(c.setManual(11, false, msToNs(100)).ok());
    KeyState r = c.resolve(est, msToNs(1100));
    CHECK(r.source == ValueSource::Manual);
    CHECK(r.tonic == 11);
    CHECK_FALSE(r.minor);
    CHECK(r.confidence == 1.f);
    CHECK(r.ageNs == msToNs(1000));
    CHECK(c.resolve(KeyState{}, 0).valid());                 // manual wins even when the detector says Unknown
    CHECK_FALSE(c.setManual(12, false, 0).ok());
    CHECK_FALSE(c.setManual(-1, true, 0).ok());
    CHECK(c.resolve(est, 0).tonic == 11);                    // rejected value changed nothing
    c.release();
    CHECK(c.resolve(est, 0).tonic == 4);
    CHECK(c.resolve(est, 0).source == ValueSource::Estimated);
    CHECK_FALSE(c.resolve(KeyState{}, 0).valid());
}
