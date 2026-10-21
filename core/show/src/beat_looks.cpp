#include "ss/show/beat_looks.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ss::show {

namespace {

// Raised cosine 0.5 - 0.5 cos(2 pi i / 256), 16-bit, one cycle in 256 steps (generated once, committed).
constexpr std::uint16_t kRaisedCos[256] = {
    0, 10, 39, 89, 158, 246, 355, 482, 630, 796, 982, 1187, 1411, 1654, 1915, 2196,
    2494, 2811, 3146, 3499, 3869, 4257, 4662, 5084, 5522, 5977, 6448, 6935, 7438, 7956, 8488, 9036,
    9597, 10173, 10762, 11365, 11980, 12608, 13248, 13900, 14563, 15237, 15922, 16616, 17321, 18035, 18758, 19489,
    20228, 20975, 21728, 22489, 23256, 24028, 24806, 25588, 26375, 27166, 27960, 28756, 29556, 30357, 31160, 31963,
    32767, 33572, 34375, 35178, 35979, 36779, 37575, 38369, 39160, 39947, 40729, 41507, 42279, 43046, 43807, 44560,
    45307, 46046, 46777, 47500, 48214, 48919, 49613, 50298, 50972, 51635, 52287, 52927, 53555, 54170, 54773, 55362,
    55938, 56499, 57047, 57579, 58097, 58600, 59087, 59558, 60013, 60451, 60873, 61278, 61666, 62036, 62389, 62724,
    63041, 63339, 63620, 63881, 64124, 64348, 64553, 64739, 64905, 65053, 65180, 65289, 65377, 65446, 65496, 65525,
    65535, 65525, 65496, 65446, 65377, 65289, 65180, 65053, 64905, 64739, 64553, 64348, 64124, 63881, 63620, 63339,
    63041, 62724, 62389, 62036, 61666, 61278, 60873, 60451, 60013, 59558, 59087, 58600, 58097, 57579, 57047, 56499,
    55938, 55362, 54773, 54170, 53555, 52927, 52287, 51635, 50972, 50298, 49613, 48919, 48214, 47500, 46777, 46046,
    45307, 44560, 43807, 43046, 42279, 41507, 40729, 39947, 39160, 38369, 37575, 36779, 35979, 35178, 34375, 33572,
    32768, 31963, 31160, 30357, 29556, 28756, 27960, 27166, 26375, 25588, 24806, 24028, 23256, 22489, 21728, 20975,
    20228, 19489, 18758, 18035, 17321, 16616, 15922, 15237, 14563, 13900, 13248, 12608, 11980, 11365, 10762, 10173,
    9597, 9036, 8488, 7956, 7438, 6935, 6448, 5977, 5522, 5084, 4662, 4257, 3869, 3499, 3146, 2811,
    2494, 2196, 1915, 1654, 1411, 1187, 982, 796, 630, 482, 355, 246, 158, 89, 39, 10,
};

constexpr std::int64_t kFlashCapNum = 3;    // flash ceiling as a rational: 3 cycles per second

bool isFlash(LfoShape s) { return s == LfoShape::Pulse || s == LfoShape::Steps; }

// cycles/s = c12 / 12 * bpm / 60 <= 3  <=>  c12 * bpm <= 3 * 12 * 60 (exact in integers when bpm is scaled)
bool withinFlashCap(int c12, double bpm) {
    return static_cast<double>(c12) * bpm <= static_cast<double>(kFlashCapNum * 12 * 60) + 1e-9;
}

}  // namespace

bool BeatLooksParams::valid(const char** why) const {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (!std::isfinite(swing) || swing < 0.f || swing > BeatLooksLimits::kMaxSwing) return fail("swing outside [0, 0.5]");
    if (!std::isfinite(duty) || duty < BeatLooksLimits::kMinDuty || duty > BeatLooksLimits::kMaxDuty) return fail("duty outside [0.05, 0.95]");
    if (stepCount < 1 || stepCount > BeatLooksLimits::kMaxSteps) return fail("stepCount outside [1, 16]");
    return true;
}

Status BeatLooks::setParams(const BeatLooksParams& p) {
    const char* why = nullptr;
    if (!p.valid(&why)) return makeError(ErrorCategory::InvalidArgument, why ? why : "invalid beat looks params");
    p_ = p;
    return okStatus();
}

int BeatLooks::cyclesPer12Beats(BeatDivision d, bool triplet) {
    static constexpr int kBase[4] = {6, 12, 24, 48};
    const int base = kBase[static_cast<int>(d)];
    return triplet ? base * 3 / 2 : base;
}

BeatLooksSample BeatLooks::sample(const BeatGrid& grid, Ns now) const {
    BeatLooksSample out;
    if (!grid.valid()) return out;

    // ---- effective shape after the strobe rules ----
    LfoShape shape = p_.shape;
    BeatDivision div = p_.division;
    bool trip = p_.triplet;
    bool capped = false;
    const double bpm = grid.bpm();
    if (p_.strobingDisabled && isFlash(shape)) {
        shape = LfoShape::Sine;
        capped = true;
    }
    if (isFlash(shape)) {
        while (!withinFlashCap(cyclesPer12Beats(div, trip), bpm)) {
            capped = true;
            if (trip) {
                trip = false;                      // drop the triplet first (a 2/3 slower rate)
            } else if (div != BeatDivision::Half) {
                div = static_cast<BeatDivision>(static_cast<int>(div) - 1);
            } else {
                shape = LfoShape::Sine;            // even a half-beat flash exceeds the ceiling: smooth fallback
                break;
            }
        }
    }
    const int c12 = cyclesPer12Beats(div, trip);
    out.shape = shape;
    out.division = div;
    out.triplet = trip;
    out.flashType = isFlash(shape);
    out.capped = capped;
    out.cyclesPerSec = static_cast<double>(c12) / 12.0 * bpm / 60.0;

    // ---- beat clock: fraction of the current beat in Q16 and the absolute beat count ----
    const Ns period = beatNs(bpm);
    const Ns nextBeat = grid.boundaryAfter(now, false);
    const Ns prevBeat = nextBeat - period;
    std::int64_t fracQ16 = (static_cast<std::int64_t>(now - prevBeat) * 65536) / period;
    if (fracQ16 < 0) fracQ16 = 0;
    if (fracQ16 > 65535) fracQ16 = 65535;
    const std::int64_t beatAbs = grid.beatAt(now);

    // Position in cycles, Q16: (beat + frac) * cyclesPerBeat.
    const std::int64_t posQ16 = BeatGrid::floorDiv((beatAbs * 65536 + fracQ16) * c12, 12);
    // Swing works on pairs of cycles: pair = 2 cycles, the second cycle starts at fraction b of the pair.
    const std::int64_t pairQ16 = 2 * 65536;
    const std::int64_t pair = BeatGrid::floorDiv(posQ16, pairQ16);
    const std::int64_t inPair = BeatGrid::mod(posQ16, pairQ16);
    const std::int64_t f = inPair / 2;                               // fraction of the pair, Q16 (0..65535)
    const std::int64_t bQ16 = 32768 + static_cast<std::int64_t>(std::lround(static_cast<double>(p_.swing) * 65536.0 / 2.0));
    std::int64_t cycleInPair = 0, local = 0;
    if (f < bQ16) {
        cycleInPair = 0;
        local = (f * 65536) / bQ16;
    } else {
        cycleInPair = 1;
        local = ((f - bQ16) * 65536) / (65536 - bQ16);
    }
    if (local < 0) local = 0;
    if (local > 65535) local = 65535;
    const std::int64_t cycle = pair * 2 + cycleInPair;
    out.cycle = cycle;

    // ---- shape value, 16-bit ----
    std::int64_t v = 0;
    switch (shape) {
        case LfoShape::Sine: {
            const std::int64_t i = local >> 8, fr = local & 255;
            const std::int64_t a = kRaisedCos[i], b2 = kRaisedCos[(i + 1) & 255];
            v = a + ((b2 - a) * fr >> 8);
            break;
        }
        case LfoShape::Triangle: v = local < 32768 ? local * 2 : (65535 - local) * 2; break;
        case LfoShape::RampUp: v = local; break;
        case LfoShape::RampDown: v = 65535 - local; break;
        case LfoShape::Pulse: {
            const std::int64_t duty = static_cast<std::int64_t>(std::lround(static_cast<double>(p_.duty) * 65536.0));
            v = local < duty ? 65535 : 0;
            break;
        }
        case LfoShape::Steps: {
            const std::int64_t idx = BeatGrid::mod(cycle, p_.stepCount);
            v = static_cast<std::int64_t>(p_.steps[static_cast<std::size_t>(idx)]) * 257;
            break;
        }
    }
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    out.value = static_cast<float>(static_cast<double>(v) / 65535.0);
    out.valid = true;
    return out;
}

}  // namespace ss::show
