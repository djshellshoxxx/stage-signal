// StageSignal palette: key-to-colour mapping (FEAT-002). Detected key -> hue wheel colour, smoothed and
// luminance-bounded. Spec: docs/specs/musical-features.md "FEAT-002". Integer-only math: same input => same colour
// on every platform. Inputs are KeyState values (base types); no dependency on the audio module.
#pragma once
#include <cstdint>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"

namespace ss::palette {

struct Rgb8 {
    std::uint8_t r = 0, g = 0, b = 0;
    friend bool operator==(const Rgb8& a, const Rgb8& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
    friend bool operator!=(const Rgb8& a, const Rgb8& b) { return !(a == b); }
};

struct KeyColorParams {
    int enterPermille = 600;        // [0, 1000] confidence a new key needs before it can become the stable key
    int holdPermille = 400;         // [0, enter] confidence that keeps the current stable key alive
    Ns confirmNs = secToNs(4);      // [1 s, 30 s] a candidate must stay confident this long to replace the stable key
    Ns staleNs = secToNs(8);        // [2 s, 120 s] no reading above holdPermille this long => neutral colour
    int maxSlewPerSec = 40;         // [1, 255] max change of each 8-bit channel per second (no flashing)
    int majorLightPermille = 550;   // [300, 700] HSL lightness of major keys
    int minorLightPermille = 350;   // [150, 500] HSL lightness of minor keys (must be below major)
    int saturationPermille = 700;   // [0, 1000] HSL saturation for all keys
    int lumMinPermille = 60;        // [0, 500]  lower bound of relative luminance Y (WCAG 2.x definition, x1000)
    int lumMaxPermille = 600;       // [lumMin + 50, 1000] upper bound of Y
    bool valid(const char** why = nullptr) const;
};

struct KeyColorLimits {
    static constexpr int kFifthDeg = 30;        // 360 / 12 positions on the circle of fifths
    static constexpr int kCoolHueDeg = 240;     // minor keys drift toward blue ...
    static constexpr int kMinorCoolDeg = 20;    // ... by at most this many degrees
};

// One instance per output route (or per show). Call update() at >= 10 Hz with a monotonic time.
class KeyColorMapper {
public:
    KeyColorMapper() { reset(); }
    Status setParams(const KeyColorParams& p);
    const KeyColorParams& params() const { return p_; }
    void reset();

    // Feed the latest key estimate (or override) at time nowNs. Invalid/Unknown keys count as no evidence.
    void update(const KeyState& key, Ns nowNs);

    Rgb8 color() const { return cur_; }                 // output: slewed toward target(), always inside the Y bounds
    Rgb8 target() const;                                // colour the output is moving toward
    bool neutral() const { return !stableValid_; }      // true while no stable key is held
    int stableTonic() const { return stableValid_ ? stableTonic_ : -1; }
    bool stableMinor() const { return stableValid_ && stableMinor_; }

    // ---- pure, deterministic helpers ----
    static int fifthsPosition(int tonic);                       // 0..11 around the circle of fifths, C = 0
    static int hueDeg(int tonic, bool minor);                   // 0..359, -1 for an invalid tonic
    static Rgb8 colorFor(int tonic, bool minor, const KeyColorParams& p);   // luminance-bounded target colour
    static Rgb8 neutralColor(const KeyColorParams& p);
    static int relativeLuminancePermille(Rgb8 c);               // Y x 1000, rounded
    static Rgb8 clampLuminance(Rgb8 c, int minPermille, int maxPermille);

private:
    void slew(Ns dt);
    KeyColorParams p_;
    Rgb8 cur_{};
    bool timed_ = false, stableValid_ = false, stableMinor_ = false, candValid_ = false, candMinor_ = false;
    int stableTonic_ = -1, candTonic_ = -1;
    Ns lastNs_ = 0, candSince_ = 0, lastEvidence_ = 0;
    std::int64_t slewRemNs_ = 0;
};

}  // namespace ss::palette
