// FEAT-003: BeatLooks (tempo-synced LFO / step values locked to the beat grid, flash ceiling 3 Hz).
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "doctest/doctest.h"
#include "ss/show/beat_grid.hpp"
#include "ss/show/beat_looks.hpp"

using namespace ss;
using namespace ss::show;

namespace {

BeatGrid gridAt(double bpm, Ns anchor = 0) {
    BeatGrid g(4);
    g.setDownbeat(anchor);
    g.setTempo(bpm, anchor, 0.01);
    return g;
}

BeatLooksParams with(BeatDivision d, bool trip, LfoShape s) {
    BeatLooksParams p;
    p.division = d;
    p.triplet = trip;
    p.shape = s;
    return p;
}

const LfoShape kShapes[] = {LfoShape::Sine, LfoShape::Triangle, LfoShape::RampUp, LfoShape::RampDown, LfoShape::Pulse, LfoShape::Steps};
const BeatDivision kDivs[] = {BeatDivision::Half, BeatDivision::Quarter, BeatDivision::Eighth, BeatDivision::Sixteenth};

}  // namespace

TEST_CASE("FEAT-003: out-of-range parameters are rejected and the previous ones kept") {
    BeatLooks b;
    BeatLooksParams bad;
    bad.swing = 0.6f;
    CHECK_FALSE(b.setParams(bad).ok());
    bad = BeatLooksParams{};
    bad.duty = 0.f;
    CHECK_FALSE(b.setParams(bad).ok());
    bad = BeatLooksParams{};
    bad.stepCount = 0;
    CHECK_FALSE(b.setParams(bad).ok());
    bad = BeatLooksParams{};
    bad.stepCount = 17;
    CHECK_FALSE(b.setParams(bad).ok());
    CHECK(b.params().stepCount == BeatLooksParams{}.stepCount);
}

TEST_CASE("FEAT-003: without a locked tempo the output is invalid and zero, never a stale value") {
    BeatLooks b;
    REQUIRE(b.setParams(with(BeatDivision::Quarter, false, LfoShape::Sine)).ok());
    BeatGrid unanchored(4);
    const BeatLooksSample a = b.sample(unanchored, msToNs(250));
    CHECK_FALSE(a.valid);
    CHECK(a.value == 0.f);
    BeatGrid anchoredNoTempo(4);
    anchoredNoTempo.setDownbeat(0);
    CHECK_FALSE(b.sample(anchoredNoTempo, msToNs(250)).valid);
}

TEST_CASE("FEAT-003: quarter-note sine peaks at the half beat and returns to zero on the beat") {
    BeatLooks b;
    REQUIRE(b.setParams(with(BeatDivision::Quarter, false, LfoShape::Sine)).ok());
    const BeatGrid g = gridAt(120.0);            // 500 ms per beat
    const BeatLooksSample on = b.sample(g, 0);
    CHECK(on.valid);
    CHECK(on.value < 0.01f);
    CHECK(b.sample(g, msToNs(250)).value > 0.99f);
    CHECK(b.sample(g, msToNs(500)).value < 0.01f);
    CHECK(std::fabs(b.sample(g, msToNs(125)).value - 0.5f) < 0.01f);
}

TEST_CASE("FEAT-003: every shape repeats exactly every 4 beats and the colour cycle advances by the rate") {
    const BeatGrid g = gridAt(120.0);
    for (LfoShape s : kShapes) {
        for (BeatDivision d : kDivs) {
            for (bool trip : {false, true}) {
                BeatLooks b;
                BeatLooksParams p = with(d, trip, s);
                p.steps[0] = 10; p.steps[1] = 200; p.steps[2] = 90; p.steps[3] = 255; p.stepCount = 4;
                REQUIRE(b.setParams(p).ok());
                for (int k = 0; k < 9; ++k) {
                    const Ns t = msToNs(37 * k + 11);
                    const BeatLooksSample x = b.sample(g, t);
                    const BeatLooksSample y = b.sample(g, t + secToNs(2));   // 4 beats later at 120 bpm
                    if (s == LfoShape::Steps) {
                        // The pattern repeats every stepCount cycles, which a 4-beat span need not be (triplets).
                        const float level = p.steps[static_cast<std::size_t>(BeatGrid::mod(x.cycle, p.stepCount))] / 255.f;
                        CHECK(std::fabs(x.value - level) < 1e-3f);
                    } else {
                        CHECK(x.value == y.value);
                    }
                    CHECK(y.cycle - x.cycle == BeatLooks::cyclesPer12Beats(x.division, x.triplet) * 4 / 12);
                }
            }
        }
    }
}

TEST_CASE("FEAT-003: triplet quarter runs at 1.5 cycles per beat and eighths at two per beat") {
    const BeatGrid g = gridAt(120.0);
    BeatLooks b;
    REQUIRE(b.setParams(with(BeatDivision::Quarter, true, LfoShape::Sine)).ok());
    const BeatLooksSample t0 = b.sample(g, 0);
    const BeatLooksSample t4 = b.sample(g, secToNs(2));
    CHECK(t4.cycle - t0.cycle == 6);             // 4 beats * 1.5
    CHECK(std::fabs(t0.cyclesPerSec - 3.0) < 1e-9);
    REQUIRE(b.setParams(with(BeatDivision::Eighth, false, LfoShape::RampUp)).ok());
    const BeatLooksSample e0 = b.sample(g, msToNs(10));
    const BeatLooksSample e1 = b.sample(g, msToNs(510));
    CHECK(e1.cycle - e0.cycle == 2);
}

TEST_CASE("FEAT-003: swing 1/3 moves the second eighth to the triplet position, straight swing keeps the midpoint") {
    const BeatGrid g = gridAt(120.0);
    BeatLooks b;
    BeatLooksParams p = with(BeatDivision::Eighth, false, LfoShape::RampUp);
    REQUIRE(b.setParams(p).ok());
    CHECK(b.sample(g, msToNs(240)).cycle == 0);
    CHECK(b.sample(g, msToNs(260)).cycle == 1);

    p.swing = 1.f / 3.f;
    REQUIRE(b.setParams(p).ok());
    CHECK(b.sample(g, msToNs(320)).cycle == 0);  // boundary at 2/3 of the beat = 333 ms
    CHECK(b.sample(g, msToNs(345)).cycle == 1);
}

TEST_CASE("FEAT-003: pulse and step shapes are capped so that no more than 3 flashes per second are produced") {
    BeatLooks b;
    REQUIRE(b.setParams(with(BeatDivision::Sixteenth, false, LfoShape::Pulse)).ok());
    const BeatLooksSample s = b.sample(gridAt(120.0), 0);     // 16ths at 120 bpm = 8 Hz
    CHECK(s.capped);
    CHECK(s.flashType);
    CHECK(s.division == BeatDivision::Quarter);                // 16th -> 8th (4 Hz) -> quarter (2 Hz)
    CHECK(s.cyclesPerSec <= BeatLooksLimits::kMaxFlashHz + 1e-9);
}

TEST_CASE("FEAT-003: over a 10 s sweep of tempi, divisions and flash shapes no more than 30 rising edges occur") {
    const double bpms[] = {40, 60, 90, 120, 150, 200, 240, 300, 360};
    int combos = 0;
    for (double bpm : bpms) {
        const BeatGrid g = gridAt(bpm);
        for (BeatDivision d : kDivs) {
            for (bool trip : {false, true}) {
                for (LfoShape s : {LfoShape::Pulse, LfoShape::Steps}) {
                    BeatLooksParams p = with(d, trip, s);
                    p.steps[0] = 255; p.steps[1] = 0; p.stepCount = 2; p.duty = 0.5f;
                    BeatLooks b;
                    REQUIRE(b.setParams(p).ok());
                    const BeatLooksSample first = b.sample(g, 0);
                    CHECK(first.cyclesPerSec <= BeatLooksLimits::kMaxFlashHz + 1e-9);
                    int edges = 0;
                    float prev = 0.f;
                    for (Ns t = 0; t < secToNs(10); t += msToNs(2)) {
                        const float v = b.sample(g, t).value;
                        if (prev < 0.5f && v >= 0.5f) ++edges;
                        prev = v;
                    }
                    CHECK(edges <= 30);
                    ++combos;
                }
            }
        }
    }
    CHECK(combos == 9 * 4 * 2 * 2);
}

TEST_CASE("FEAT-003: above 360 bpm a pulse falls back to a smooth shape instead of flashing faster than 3 Hz") {
    BeatLooks b;
    REQUIRE(b.setParams(with(BeatDivision::Half, false, LfoShape::Pulse)).ok());
    const BeatLooksSample s = b.sample(gridAt(400.0), 0);
    CHECK(s.capped);
    CHECK_FALSE(s.flashType);
    CHECK(s.shape == LfoShape::Sine);
}

TEST_CASE("FEAT-003: strobing disabled turns pulse into smooth sine and no flash shape reaches the output") {
    BeatLooks b;
    BeatLooksParams p = with(BeatDivision::Quarter, false, LfoShape::Pulse);
    p.strobingDisabled = true;
    REQUIRE(b.setParams(p).ok());
    const BeatLooksSample s = b.sample(gridAt(120.0), msToNs(125));
    CHECK(s.capped);
    CHECK_FALSE(s.flashType);
    CHECK(s.value > 0.3f);
    CHECK(s.value < 0.7f);
}

TEST_CASE("FEAT-003: step pattern gives one level per cycle and wraps at the step count") {
    BeatLooks b;
    BeatLooksParams p = with(BeatDivision::Quarter, false, LfoShape::Steps);
    p.stepCount = 3;
    p.steps[0] = 0; p.steps[1] = 128; p.steps[2] = 255;
    REQUIRE(b.setParams(p).ok());
    const BeatGrid g = gridAt(120.0);
    const float expect[] = {0.f, 128.f / 255.f, 1.f, 0.f, 128.f / 255.f, 1.f};
    for (int c = 0; c < 6; ++c) {
        const BeatLooksSample s = b.sample(g, msToNs(500 * c + 100));
        CHECK(s.cycle == c);
        CHECK(std::fabs(s.value - expect[c]) < 1e-3f);
    }
}

TEST_CASE("FEAT-003: output is always within 0..1 and identical for identical inputs") {
    BeatLooks b;
    const BeatGrid g = gridAt(133.7);
    for (LfoShape s : kShapes) {
        REQUIRE(b.setParams(with(BeatDivision::Eighth, true, s)).ok());
        for (Ns t = 0; t < secToNs(3); t += msToNs(7)) {
            const BeatLooksSample a = b.sample(g, t);
            const BeatLooksSample c = b.sample(g, t);
            CHECK(a.value >= 0.f);
            CHECK(a.value <= 1.f);
            CHECK(a.value == c.value);
            CHECK(a.cycle == c.cycle);
        }
    }
}
