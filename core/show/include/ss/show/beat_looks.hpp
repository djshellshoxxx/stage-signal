// StageSignal show: beat-synced modulation (FEAT-003). Tempo-synced LFO / step pattern values (0..1) for
// intensity and colour steps. Spec: docs/specs/musical-features.md "FEAT-003".
// The output is a pure function of (BeatGrid, now, params): every consumer (lighting, render) calls sample() and
// gets the same value at the same instant. Integer phase math, so the output is identical on every platform.
#pragma once
#include <cstdint>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/show/beat_grid.hpp"

namespace ss::show {

enum class LfoShape : std::uint8_t { Sine, Triangle, RampUp, RampDown, Pulse, Steps };
enum class BeatDivision : std::uint8_t { Half, Quarter, Eighth, Sixteenth };

struct BeatLooksLimits {
    static constexpr double kMaxFlashHz = 3.0;   // flash ceiling: Pulse and Steps never exceed it (WCAG 2.3.1 general flash)
    static constexpr int kMaxSteps = 16;
    static constexpr float kMaxSwing = 0.5f;     // 0 = straight; 1/3 = triplet feel
    static constexpr float kMinDuty = 0.05f, kMaxDuty = 0.95f;
};

struct BeatLooksParams {
    BeatDivision division = BeatDivision::Quarter;
    bool triplet = false;                 // scales the rate by 3/2 (e.g. quarter triplets = 1.5 cycles per beat)
    LfoShape shape = LfoShape::Sine;
    float swing = 0.f;                    // [0, 0.5]: the second cycle of each pair starts at 0.5 + swing/2 of the pair
    float duty = 0.5f;                    // [0.05, 0.95] Pulse only: fraction of the cycle that is high
    std::uint8_t steps[BeatLooksLimits::kMaxSteps] = {};   // Steps only: cycle c uses steps[c % stepCount], 0..255
    int stepCount = 4;                    // [1, 16]
    bool strobingDisabled = false;        // mirrors ShowSnapshotLite::strobingDisabled: Pulse/Steps become Sine
    bool valid(const char** why = nullptr) const;
};

struct BeatLooksSample {
    float value = 0.f;                    // 0..1 modulation (LFO or step level); 0 when not valid
    std::int64_t cycle = 0;               // absolute cycle index since the grid lineage; use for colour steps
    bool valid = false;                   // false when the grid is not anchored with a tempo
    bool flashType = false;               // effective shape is Pulse or Steps
    bool capped = false;                  // division, triplet or shape was reduced to respect the flash ceiling
    LfoShape shape = LfoShape::Sine;      // effective shape
    BeatDivision division = BeatDivision::Quarter;   // effective division
    bool triplet = false;                 // effective triplet flag
    double cyclesPerSec = 0.0;            // effective rate
};

class BeatLooks {
public:
    // Rejects out-of-range parameters with InvalidArgument and keeps the previous ones.
    Status setParams(const BeatLooksParams& p);
    const BeatLooksParams& params() const { return p_; }

    // Value at time `now` for the grid. Stateless: no allocation, no clock.
    BeatLooksSample sample(const BeatGrid& grid, Ns now) const;

    // Cycles per beat times 12 for a division (exact integer): Half 6, Quarter 12, Eighth 24, Sixteenth 48,
    // triplet 3/2 of those. Exposed for tests and the UI label.
    static int cyclesPer12Beats(BeatDivision d, bool triplet);

private:
    BeatLooksParams p_;
};

}  // namespace ss::show
