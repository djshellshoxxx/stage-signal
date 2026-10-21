// StageSignal audio: Energy Arc (musical feature, DEC-13). Long-term energy envelope plus build/drop phase.
// Spec: docs/specs/audio.md "Energy Arc". Deterministic: same input sequence => same output on one build.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "ss/base/error.hpp"

namespace ss::audio {

enum class EnergyPhase : std::uint8_t { Steady = 0, Build = 1, Drop = 2 };   // value stored in FeatureSnapshot::arcPhase

// Tunables. Every field has a documented valid range; EnergyArc::setParams() rejects (and keeps the old
// parameters) when any field is out of range or the cross constraints fail.
struct EnergyArcParams {
    double levelFloorDb = -50.0;    // [-90, -20]  total band power (dB, full-scale sine = 0 dB) mapped to level 0
    double levelCeilDb = 0.0;       // [-30, 6]    ... mapped to level 1 (ceil - floor must be >= 20 dB)
    double fastTauS = 1.0;          // [0.3, 3]    "fast" tracker used for build/dip logic
    double arcTauS = 8.0;           // [4, 30]     "slow" tracker; this is the published energyArc
    double buildRise = 0.08;        // [0.03, 0.4] rise of the fast tracker (level units) over buildWindowS to enter Build
    double buildWindowS = 6.0;      // [3, 12]
    double dropJump = 0.20;         // [0.08, 0.6] short-tracker rise above its 4 s minimum that counts as a sudden return
    double dropDipDepth = 0.10;     // [0.03, 0.4] required depth of the dip before a drop (fast-tracker peak before the dip minus the minimum)
    double dropHoldS = 4.0;         // [1, 10]     time the Drop phase is held
    double refractoryS = 8.0;       // [2, 30]     minimum time between two Drop phases
    bool valid(const char** why = nullptr) const;
};

class EnergyArc {
public:
    // framesPerSecond: rate of push() calls (analysis hops, e.g. 187.5) in [5, 2000].
    bool prepare(double framesPerSecond);
    void reset();
    Status setParams(const EnergyArcParams& p);
    const EnergyArcParams& params() const { return p_; }

    // One call per analysis frame. power = total band power (sum of the three bands, full-scale sine = 1).
    // Non-finite or negative power counts as 0. silent forces level 0. Allocation free.
    void push(double power, bool silent);

    float arc() const { return arc_; }                      // 0..1 (slow tracker)
    EnergyPhase phase() const { return phase_; }
    double fastLevel() const { return f_; }                 // introspection for tests/diagnostics
    std::uint64_t dropCount() const { return drops_; }

    static constexpr double kTickHz = 20.0;                 // internal control rate
    static constexpr double kShortTauS = 0.25;              // "short" tracker for sudden-return detection
    static constexpr double kMinBuildDwellS = 2.0;
    static constexpr double kBuildContinueSlopeS = 3.0;     // look-back for staying in Build

private:
    void tick(double power);
    double back(const std::array<float, 256>& ring, double seconds) const;

    EnergyArcParams p_;
    double hopsPerTick_ = 1.0, tickPhase_ = 0.0, acc_ = 0.0, accN_ = 0.0;
    double aShort_ = 0, aFast_ = 0, aArc_ = 0;
    double s_ = 0, f_ = 0, m_ = 0;
    std::array<float, 256> fHist_{}, sHist_{};
    std::size_t w_ = 0, filled_ = 0;
    EnergyPhase phase_ = EnergyPhase::Steady;
    std::uint64_t tickNo_ = 0, phaseSinceTick_ = 0, lastDropTick_ = 0, drops_ = 0;
    bool everDropped_ = false, prepared_ = false;
    float arc_ = 0.f;
};

}  // namespace ss::audio
