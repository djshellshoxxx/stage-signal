#include "ss/audio/energy_arc.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace ss::audio {

namespace {
bool inRange(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }
}

bool EnergyArcParams::valid(const char** why) const {
    auto bad = [&](const char* m) { if (why) *why = m; return false; };
    if (!inRange(levelFloorDb, -90, -20)) return bad("levelFloorDb");
    if (!inRange(levelCeilDb, -30, 6)) return bad("levelCeilDb");
    if (levelCeilDb - levelFloorDb < 20.0) return bad("ceil-floor span");
    if (!inRange(fastTauS, 0.3, 3)) return bad("fastTauS");
    if (!inRange(arcTauS, 4, 30)) return bad("arcTauS");
    if (!inRange(buildRise, 0.03, 0.4)) return bad("buildRise");
    if (!inRange(buildWindowS, 3, 12)) return bad("buildWindowS");
    if (!inRange(dropJump, 0.08, 0.6)) return bad("dropJump");
    if (!inRange(dropDipDepth, 0.03, 0.4)) return bad("dropDipDepth");
    if (!inRange(dropHoldS, 1, 10)) return bad("dropHoldS");
    if (!inRange(refractoryS, 2, 30)) return bad("refractoryS");
    return true;
}

bool EnergyArc::prepare(double fps) {
    if (!inRange(fps, 5.0, 2000.0)) return false;
    hopsPerTick_ = fps / kTickHz;
    prepared_ = true;
    (void)setParams(p_);
    reset();
    return true;
}

Status EnergyArc::setParams(const EnergyArcParams& p) {
    const char* why = "";
    if (!p.valid(&why)) return makeError(ErrorCategory::InvalidArgument, std::string("EnergyArcParams: ") + why);
    p_ = p;
    const double dt = 1.0 / kTickHz;
    aShort_ = 1.0 - std::exp(-dt / kShortTauS);
    aFast_ = 1.0 - std::exp(-dt / p_.fastTauS);
    aArc_ = 1.0 - std::exp(-dt / p_.arcTauS);
    return okStatus();
}

void EnergyArc::reset() {
    tickPhase_ = acc_ = accN_ = 0.0;
    s_ = f_ = m_ = 0.0;
    fHist_.fill(0.f); sHist_.fill(0.f);
    w_ = filled_ = 0;
    phase_ = EnergyPhase::Steady;
    tickNo_ = phaseSinceTick_ = lastDropTick_ = drops_ = 0;
    everDropped_ = false;
    arc_ = 0.f;
}

void EnergyArc::push(double power, bool silent) {
    if (!prepared_) return;
    if (!std::isfinite(power) || power < 0.0) power = 0.0;
    if (silent) power = 0.0;
    acc_ += power; accN_ += 1.0; tickPhase_ += 1.0;
    if (tickPhase_ >= hopsPerTick_) {
        tickPhase_ -= hopsPerTick_;
        tick(acc_ / accN_);
        acc_ = 0.0; accN_ = 0.0;
    }
}

// Value of ring entry `seconds` before the newest one (clamped to what exists). w_ = next write index.
double EnergyArc::back(const std::array<float, 256>& ring, double seconds) const {
    if (filled_ == 0) return 0.0;
    std::size_t k = static_cast<std::size_t>(seconds * kTickHz + 0.5);
    k = std::min<std::size_t>(k, std::min<std::size_t>(filled_ - 1, 255));
    return ring[(w_ + 255 - k) & 255];
}
void EnergyArc::tick(double power) {
    ++tickNo_;
    const double db = power > 1e-12 ? 10.0 * std::log10(power) : -120.0;
    const double x = std::clamp((db - p_.levelFloorDb) / (p_.levelCeilDb - p_.levelFloorDb), 0.0, 1.0);
    s_ += aShort_ * (x - s_);
    f_ += aFast_ * (x - f_);
    m_ += aArc_ * (x - m_);
    fHist_[w_] = static_cast<float>(f_);
    sHist_[w_] = static_cast<float>(s_);
    w_ = (w_ + 1) & 255;
    if (filled_ < 256) ++filled_;
    arc_ = static_cast<float>(std::clamp(m_, 0.0, 1.0));

    const double sinceS = static_cast<double>(tickNo_ - phaseSinceTick_) / kTickHz;
    const bool refractoryOver = !everDropped_ ||
        static_cast<double>(tickNo_ - lastDropTick_) / kTickHz >= p_.refractoryS;

    // Drop: sudden large energy return. The short tracker jumped >= dropJump above its 4 s minimum, at least
    // half of that within the last 0.5 s, and either we have been in Build for >= kMinBuildDwellS or a dip preceded the jump: the fast tracker
    // peak *before* the (newest) 4 s minimum, looking back 12 s, exceeds that minimum by >= dropDipDepth. Music
    // that starts from silence therefore does not count as a drop (nothing precedes the minimum). Needs 4 s of history.
    if (phase_ != EnergyPhase::Drop && refractoryOver && filled_ >= static_cast<std::size_t>(4 * kTickHz)) {
        const std::size_t n4 = static_cast<std::size_t>(4 * kTickHz);
        double sMin = 1e9; std::size_t kMin = 1;
        for (std::size_t k = 1; k <= n4; ++k) {
            const double v = sHist_[(w_ + 256 - k) & 255];
            if (v < sMin) { sMin = v; kMin = k; }
        }
        const double jump = s_ - sMin;
        const double recentJump = s_ - back(sHist_, 0.5);
        if (jump >= p_.dropJump && recentJump >= 0.5 * p_.dropJump) {
            double peakBefore = 0.0;
            const std::size_t kMax = std::min<std::size_t>(filled_, static_cast<std::size_t>(12 * kTickHz));
            for (std::size_t k = kMin + 1; k <= kMax; ++k) peakBefore = std::max<double>(peakBefore, fHist_[(w_ + 256 - k) & 255]);
            if ((phase_ == EnergyPhase::Build && sinceS >= kMinBuildDwellS) || peakBefore - sMin >= p_.dropDipDepth) {
                phase_ = EnergyPhase::Drop; phaseSinceTick_ = tickNo_; lastDropTick_ = tickNo_;
                everDropped_ = true; ++drops_;
                return;
            }
        }
    }
    if (phase_ == EnergyPhase::Drop) {
        if (sinceS >= p_.dropHoldS) { phase_ = EnergyPhase::Steady; phaseSinceTick_ = tickNo_; }
        return;
    }
    // Build: sustained rise of the fast tracker. Enter: rise >= buildRise over buildWindowS and still rising
    // (>= 0.02 over the last 2 s). Stay while the last 3 s slope is >= a quarter of the entry rate (hysteresis)
    // and for at least kMinBuildDwellS.
    const bool haveWindow = filled_ > static_cast<std::size_t>(p_.buildWindowS * kTickHz);
    if (phase_ == EnergyPhase::Steady) {
        if (haveWindow && f_ - back(fHist_, p_.buildWindowS) >= p_.buildRise && f_ - back(fHist_, 2.0) >= 0.02) {
            phase_ = EnergyPhase::Build; phaseSinceTick_ = tickNo_;
        }
    } else if (phase_ == EnergyPhase::Build && sinceS >= kMinBuildDwellS) {
        const double keep = 0.25 * (p_.buildRise / p_.buildWindowS) * kBuildContinueSlopeS;
        if (f_ - back(fHist_, kBuildContinueSlopeS) < keep) { phase_ = EnergyPhase::Steady; phaseSinceTick_ = tickNo_; }
    }
}

}  // namespace ss::audio
