// FEAT-002: KeyColorMapper (key -> hue wheel colour, minor darker/cooler, neutral on low confidence,
// confirmation and slew smoothing, luminance bounds).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "doctest/doctest.h"
#include "ss/palette/key_color.hpp"

using namespace ss;
using namespace ss::palette;

namespace {

KeyState key(int tonic, bool minor, float conf) {
    KeyState k;
    k.tonic = tonic;
    k.minor = minor;
    k.confidence = conf;
    k.source = ValueSource::Estimated;
    return k;
}

int lum(Rgb8 c) { return KeyColorMapper::relativeLuminancePermille(c); }

int circularDist(int a, int b) {
    const int d = std::abs(a - b) % 360;
    return d > 180 ? 360 - d : d;
}

// Feed `n` updates at 100 ms steps, starting at *t, with a fixed key reading.
void feed(KeyColorMapper& m, Ns* t, int n, const KeyState& k) {
    for (int i = 0; i < n; ++i) {
        *t += msToNs(100);
        m.update(k, *t);
    }
}

}  // namespace

TEST_CASE("FEAT-002: invalid colour parameters are rejected and the previous ones kept") {
    KeyColorMapper m;
    KeyColorParams bad;
    bad.minorLightPermille = 600;               // not below major (550)
    CHECK_FALSE(m.setParams(bad).ok());
    bad = KeyColorParams{};
    bad.holdPermille = 700;                     // hold above enter
    CHECK_FALSE(m.setParams(bad).ok());
    bad = KeyColorParams{};
    bad.lumMaxPermille = bad.lumMinPermille + 10;
    CHECK_FALSE(m.setParams(bad).ok());
    bad = KeyColorParams{};
    bad.maxSlewPerSec = 0;
    CHECK_FALSE(m.setParams(bad).ok());
    CHECK(m.params().minorLightPermille == KeyColorParams{}.minorLightPermille);
    CHECK(m.setParams(KeyColorParams{}).ok());
}

TEST_CASE("FEAT-002: circle of fifths places adjacent fifths next to each other on the hue wheel") {
    for (int t = 0; t < 12; ++t) {
        const int next = (t + 7) % 12;          // the dominant of t
        const int d = std::abs(KeyColorMapper::fifthsPosition(next) - KeyColorMapper::fifthsPosition(t));
        CHECK((d == 1 || d == 11));
        const int dh = circularDist(KeyColorMapper::hueDeg(next, false), KeyColorMapper::hueDeg(t, false));
        CHECK(dh == 30);
    }
    CHECK(KeyColorMapper::hueDeg(0, false) == 0);     // C major sits at 0 degrees
    CHECK(KeyColorMapper::hueDeg(7, false) == 30);    // G major
    CHECK(KeyColorMapper::hueDeg(2, false) == 60);    // D major
    CHECK(KeyColorMapper::hueDeg(12, false) == -1);   // invalid tonic
}

TEST_CASE("FEAT-002: minor keys are darker and cooler than the major key on the same tonic") {
    const KeyColorParams p;
    for (int t = 0; t < 12; ++t) {
        const Rgb8 maj = KeyColorMapper::colorFor(t, false, p);
        const Rgb8 min = KeyColorMapper::colorFor(t, true, p);
        CHECK(lum(min) < lum(maj));
        CHECK(circularDist(KeyColorMapper::hueDeg(t, true), 240) <= circularDist(KeyColorMapper::hueDeg(t, false), 240));
        CHECK(circularDist(KeyColorMapper::hueDeg(t, true), KeyColorMapper::hueDeg(t, false)) <= KeyColorLimits::kMinorCoolDeg);
    }
}

TEST_CASE("FEAT-002: colour is integer-deterministic and matches the documented C major value") {
    const KeyColorParams p;
    const Rgb8 c = KeyColorMapper::colorFor(0, false, p);
    // HSL(0 deg, 70%, 55%) = chroma 630, m 235 permille -> (221, 60, 60); inside the luminance bounds unchanged.
    CHECK(c == Rgb8{221, 60, 60});
    for (int i = 0; i < 100; ++i) CHECK(KeyColorMapper::colorFor(i % 12, i % 2 == 0, p) == KeyColorMapper::colorFor(i % 12, i % 2 == 0, p));
}

TEST_CASE("FEAT-002: every target colour stays inside the luminance bounds, also for extreme parameters") {
    KeyColorParams extremes[3];
    extremes[0].majorLightPermille = 700;
    extremes[0].lumMaxPermille = 300;           // bright keys must be pulled down
    extremes[1].minorLightPermille = 150;
    extremes[1].lumMinPermille = 120;           // dark keys must be lifted
    extremes[2].saturationPermille = 1000;
    extremes[2].lumMinPermille = 200;
    extremes[2].lumMaxPermille = 450;
    for (const KeyColorParams& p : extremes) {
        REQUIRE(p.valid());
        for (int t = 0; t < 12; ++t) {
            for (bool minor : {false, true}) {
                const int y = lum(KeyColorMapper::colorFor(t, minor, p));
                CHECK(y >= p.lumMinPermille - 10);
                CHECK(y <= p.lumMaxPermille + 10);
            }
        }
        CHECK(lum(KeyColorMapper::neutralColor(p)) <= p.lumMaxPermille + 10);
        CHECK(lum(KeyColorMapper::neutralColor(p)) >= p.lumMinPermille - 10);
    }
    const Rgb8 white = KeyColorMapper::clampLuminance(Rgb8{255, 255, 255}, 60, 600);
    CHECK(lum(white) <= 610);
    CHECK(lum(white) >= 590);
    const Rgb8 black = KeyColorMapper::clampLuminance(Rgb8{0, 0, 0}, 60, 600);
    CHECK(lum(black) >= 50);
    CHECK(lum(black) <= 70);
    CHECK(KeyColorMapper::clampLuminance(Rgb8{100, 120, 90}, 0, 1000) == Rgb8{100, 120, 90});
}

TEST_CASE("FEAT-002: a new key must stay confident for the confirmation time before it becomes the target") {
    KeyColorMapper m;
    Ns t = 0;
    feed(m, &t, 60, key(0, false, 0.9f));           // 6 s of C major: confirmed after 4 s
    REQUIRE_FALSE(m.neutral());
    const Rgb8 cMajor = m.target();
    CHECK(cMajor == KeyColorMapper::colorFor(0, false, m.params()));

    feed(m, &t, 39, key(7, false, 0.9f));           // 3.9 s of G major: not yet
    CHECK(m.target() == cMajor);
    feed(m, &t, 3, key(7, false, 0.9f));            // 4.2 s in total: confirmed
    CHECK(m.stableTonic() == 7);
    CHECK(m.target() == KeyColorMapper::colorFor(7, false, m.params()));
}

TEST_CASE("FEAT-002: low confidence keeps the last stable colour, then falls back to neutral") {
    KeyColorMapper m;
    Ns t = 0;
    feed(m, &t, 60, key(0, false, 0.9f));
    REQUIRE(m.stableTonic() == 0);
    feed(m, &t, 200, key(5, true, 0.5f));          // between hold (0.4) and enter (0.6): keeps C major, never switches
    CHECK(m.stableTonic() == 0);
    CHECK_FALSE(m.stableMinor());
    feed(m, &t, 50, key(0, false, 0.2f));          // below hold for 5 s: still within the 8 s stale window
    CHECK(m.stableTonic() == 0);
    feed(m, &t, 40, key(0, false, 0.2f));          // 9 s below hold: neutral
    CHECK(m.neutral());
    CHECK(m.target() == KeyColorMapper::neutralColor(m.params()));
}

TEST_CASE("FEAT-002: an unknown key counts as no evidence and goes neutral after the stale time") {
    KeyColorMapper m;
    Ns t = 0;
    feed(m, &t, 60, key(9, true, 0.95f));
    REQUIRE_FALSE(m.neutral());
    KeyState unknown;                              // source Unknown
    feed(m, &t, 90, unknown);
    CHECK(m.neutral());
}

TEST_CASE("FEAT-002: a key that flickers faster than the confirmation time never changes the target") {
    KeyColorMapper m;
    Ns t = 0;
    const Rgb8 start = m.target();
    for (int i = 0; i < 600; ++i) {                // 60 s, alternating C and G every 100 ms
        t += msToNs(100);
        m.update(key(i % 2 == 0 ? 0 : 7, false, 0.95f), t);
        CHECK(m.target() == start);
        CHECK(m.neutral());
    }
}

TEST_CASE("FEAT-002: output colour moves at most maxSlewPerSec per channel and converges to the target") {
    KeyColorMapper m;
    Ns t = 0;
    Rgb8 prev = m.color();
    int maxStep = 0;
    for (int i = 0; i < 200; ++i) {                // 20 s
        t += msToNs(100);
        m.update(key(0, false, 0.95f), t);
        const Rgb8 c = m.color();
        maxStep = std::max(maxStep, std::abs(int(c.r) - int(prev.r)));
        maxStep = std::max(maxStep, std::abs(int(c.g) - int(prev.g)));
        maxStep = std::max(maxStep, std::abs(int(c.b) - int(prev.b)));
        prev = c;
    }
    CHECK(maxStep <= 5);                           // 40 per second over 100 ms, plus one code of rounding
    CHECK(m.color() == KeyColorMapper::colorFor(0, false, m.params()));
}

TEST_CASE("FEAT-002: reset returns to the neutral colour and drops any held key") {
    KeyColorMapper m;
    Ns t = 0;
    feed(m, &t, 60, key(2, false, 0.9f));
    REQUIRE_FALSE(m.neutral());
    m.reset();
    CHECK(m.neutral());
    CHECK(m.color() == KeyColorMapper::neutralColor(m.params()));
}
