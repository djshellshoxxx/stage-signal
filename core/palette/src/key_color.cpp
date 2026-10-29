#include "ss/palette/key_color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>

namespace ss::palette {

namespace {

// sRGB 8-bit code -> linear light, 16-bit fixed point (round(65535 * EOTF(v / 255))). Generated once from the
// sRGB transfer function and committed so that no floating point runs at update time.
constexpr std::uint16_t kLinear[256] = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313,
    340, 367, 396, 427, 458, 491, 526, 562, 599, 637, 677, 718, 761, 805, 851, 898,
    947, 997, 1048, 1101, 1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
    1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258,
    3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129, 4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124,
    5257, 5392, 5530, 5669, 5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
    7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417,
    10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
    14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
    28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
    48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Linear 16-bit value -> nearest 8-bit sRGB code (table is monotone, so a binary search suffices).
int encode(std::int64_t v) {
    if (v <= 0) return 0;
    if (v >= 65535) return 255;
    const auto it = std::lower_bound(kLinear, kLinear + 256, static_cast<int>(v));
    const int i = static_cast<int>(it - kLinear);
    if (i == 0) return 0;
    if (i >= 256) return 255;
    const std::int64_t below = kLinear[i - 1], above = kLinear[i];
    return (v - below <= above - v) ? i - 1 : i;
}

// Relative luminance in 16-bit fixed point, Rec.709 / WCAG weights (2126, 7152, 722 of 10000).
std::int64_t luminance16(Rgb8 c) {
    return (2126 * std::int64_t(kLinear[c.r]) + 7152 * std::int64_t(kLinear[c.g]) + 722 * std::int64_t(kLinear[c.b])) / 10000;
}

bool finite(float f) { return std::isfinite(f); }

int permille(float f) {
    if (!finite(f)) return 0;
    return clampInt(static_cast<int>(std::lround(static_cast<double>(f) * 1000.0)), 0, 1000);
}

std::uint8_t stepChannel(std::uint8_t cur, std::uint8_t goal, std::int64_t steps) {
    const std::int64_t d = std::int64_t(goal) - cur;
    if (d == 0 || steps <= 0) return cur;
    const std::int64_t m = std::min<std::int64_t>(d < 0 ? -d : d, steps);
    return static_cast<std::uint8_t>(cur + (d < 0 ? -m : m));
}

}  // namespace

bool KeyColorParams::valid(const char** why) const {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (enterPermille < 0 || enterPermille > 1000) return fail("enterPermille outside [0, 1000]");
    if (holdPermille < 0 || holdPermille > enterPermille) return fail("holdPermille must be in [0, enterPermille]");
    if (confirmNs < secToNs(1) || confirmNs > secToNs(30)) return fail("confirmNs outside [1 s, 30 s]");
    if (staleNs < secToNs(2) || staleNs > secToNs(120)) return fail("staleNs outside [2 s, 120 s]");
    if (maxSlewPerSec < 1 || maxSlewPerSec > 255) return fail("maxSlewPerSec outside [1, 255]");
    if (majorLightPermille < 300 || majorLightPermille > 700) return fail("majorLightPermille outside [300, 700]");
    if (minorLightPermille < 150 || minorLightPermille > 500) return fail("minorLightPermille outside [150, 500]");
    if (minorLightPermille >= majorLightPermille) return fail("minor lightness must be below major lightness");
    if (saturationPermille < 0 || saturationPermille > 1000) return fail("saturationPermille outside [0, 1000]");
    if (lumMinPermille < 0 || lumMinPermille > 500) return fail("lumMinPermille outside [0, 500]");
    if (lumMaxPermille < lumMinPermille + 50 || lumMaxPermille > 1000) return fail("lumMaxPermille outside [lumMin + 50, 1000]");
    return true;
}

Status KeyColorMapper::setParams(const KeyColorParams& p) {
    const char* why = nullptr;
    if (!p.valid(&why)) return makeError(ErrorCategory::InvalidArgument, why ? why : "invalid key colour params");
    p_ = p;
    cur_ = clampLuminance(cur_, p_.lumMinPermille, p_.lumMaxPermille);
    return okStatus();
}

void KeyColorMapper::reset() {
    timed_ = stableValid_ = stableMinor_ = candValid_ = candMinor_ = false;
    stableTonic_ = candTonic_ = -1;
    lastNs_ = candSince_ = lastEvidence_ = 0;
    slewRemNs_ = 0;
    cur_ = neutralColor(p_);
}

int KeyColorMapper::fifthsPosition(int tonic) {
    return (tonic * 7) % 12;
}

int KeyColorMapper::hueDeg(int tonic, bool minor) {
    if (tonic < 0 || tonic > 11) return -1;
    int h = fifthsPosition(tonic) * KeyColorLimits::kFifthDeg;
    if (minor) {
        int d = KeyColorLimits::kCoolHueDeg - h;            // shortest signed arc toward blue
        if (d > 180) d -= 360;
        if (d <= -180) d += 360;
        d = clampInt(d, -KeyColorLimits::kMinorCoolDeg, KeyColorLimits::kMinorCoolDeg);
        h = (h + d + 360) % 360;
    }
    return h;
}

Rgb8 KeyColorMapper::colorFor(int tonic, bool minor, const KeyColorParams& p) {
    const int h = hueDeg(tonic, minor);
    if (h < 0) return neutralColor(p);
    const int L = minor ? p.minorLightPermille : p.majorLightPermille;
    const int S = p.saturationPermille;
    const int C = (1000 - std::abs(2 * L - 1000)) * S / 1000;   // HSL chroma (permille)
    const int X = C * (60 - std::abs(h % 120 - 60)) / 60;      // second largest component
    const int m = std::max(0, L - C / 2);
    int r = 0, g = 0, b = 0;
    switch (h / 60) {
        case 0: r = C; g = X; b = 0; break;
        case 1: r = X; g = C; b = 0; break;
        case 2: r = 0; g = C; b = X; break;
        case 3: r = 0; g = X; b = C; break;
        case 4: r = X; g = 0; b = C; break;
        default: r = C; g = 0; b = X; break;
    }
    auto enc = [m](int v) { return static_cast<std::uint8_t>(clampInt(((v + m) * 255 + 500) / 1000, 0, 255)); };
    return clampLuminance(Rgb8{enc(r), enc(g), enc(b)}, p.lumMinPermille, p.lumMaxPermille);
}

Rgb8 KeyColorMapper::neutralColor(const KeyColorParams& p) {
    return clampLuminance(Rgb8{200, 200, 200}, p.lumMinPermille, p.lumMaxPermille);
}

int KeyColorMapper::relativeLuminancePermille(Rgb8 c) {
    return static_cast<int>((luminance16(c) * 1000 + 32767) / 65535);
}

Rgb8 KeyColorMapper::clampLuminance(Rgb8 c, int minPermille, int maxPermille) {
    const std::int64_t yMin = (std::int64_t(minPermille) * 65535 + 500) / 1000;
    const std::int64_t yMax = (std::int64_t(maxPermille) * 65535 + 500) / 1000;
    std::int64_t R = kLinear[c.r], G = kLinear[c.g], B = kLinear[c.b];
    const std::int64_t Y = (2126 * R + 7152 * G + 722 * B) / 10000;
    if (Y > yMax) {                                   // scale linear light down to the upper bound
        R = R * yMax / Y; G = G * yMax / Y; B = B * yMax / Y;
    } else if (Y < yMin) {                            // lift all channels equally (Y rises by the same amount)
        const std::int64_t lift = yMin - Y;
        R = std::min<std::int64_t>(65535, R + lift);
        G = std::min<std::int64_t>(65535, G + lift);
        B = std::min<std::int64_t>(65535, B + lift);
    } else {
        return c;
    }
    return Rgb8{static_cast<std::uint8_t>(encode(R)), static_cast<std::uint8_t>(encode(G)), static_cast<std::uint8_t>(encode(B))};
}

Rgb8 KeyColorMapper::target() const {
    return stableValid_ ? colorFor(stableTonic_, stableMinor_, p_) : neutralColor(p_);
}

void KeyColorMapper::update(const KeyState& key, Ns nowNs) {
    if (!timed_) {
        timed_ = true;
        lastNs_ = nowNs;
        lastEvidence_ = nowNs;
    }
    const Ns dt = std::min<Ns>(std::max<Ns>(nowNs - lastNs_, 0), secToNs(60));
    if (nowNs > lastNs_) lastNs_ = nowNs;

    const bool valid = key.valid() && key.tonic >= 0 && key.tonic <= 11;
    const int conf = valid ? permille(key.confidence) : 0;
    const bool confident = valid && conf >= p_.enterPermille;
    const bool held = valid && conf >= p_.holdPermille;

    if (confident) {
        const bool isStable = stableValid_ && key.tonic == stableTonic_ && (key.minor == stableMinor_);
        if (isStable) {
            candValid_ = false;
        } else {
            if (!candValid_ || key.tonic != candTonic_ || key.minor != candMinor_) {
                candValid_ = true;
                candTonic_ = key.tonic;
                candMinor_ = key.minor;
                candSince_ = nowNs;
            }
            if (nowNs - candSince_ >= p_.confirmNs) {
                stableValid_ = true;
                stableTonic_ = key.tonic;
                stableMinor_ = key.minor;
                candValid_ = false;
            }
        }
    } else {
        candValid_ = false;                           // a dip below enterPermille restarts the confirmation
    }
    if (held) lastEvidence_ = nowNs;
    if (stableValid_ && nowNs - lastEvidence_ > p_.staleNs) stableValid_ = false;

    slew(dt);
}

void KeyColorMapper::slew(Ns dt) {
    slewRemNs_ += dt * p_.maxSlewPerSec;              // budget in (channel units * ns)
    const std::int64_t steps = slewRemNs_ / kNsPerSec;
    slewRemNs_ -= steps * kNsPerSec;
    const Rgb8 goal = target();
    Rgb8 next{stepChannel(cur_.r, goal.r, steps), stepChannel(cur_.g, goal.g, steps), stepChannel(cur_.b, goal.b, steps)};
    cur_ = clampLuminance(next, p_.lumMinPermille, p_.lumMaxPermille);
}

}  // namespace ss::palette
