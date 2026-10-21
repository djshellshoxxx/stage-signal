// StageSignal perf (MOD-14): capability admission (section 7 "Capability admission", IF-09) and the 4 h soak
// monitor (REQ-X06). Admission cannot guarantee future performance; it only reports what the recorded
// evidence supports, with reasons.
#pragma once
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"
#include "ss/perf/benchmark.hpp"
#include "ss/perf/gates.hpp"
#include "ss/perf/tiers.hpp"

namespace ss::perf {

// ---- benchmark profile (what admission needs from a SuiteReport) -----------------------------
struct ProfileScenario {
    std::string id;
    Verdict verdict = Verdict::Incomplete;
    bool metricsAvailable = false;
    double fps = 0, p95FrameMs = 0, cpuPct = 0;       // median-run values
    std::uint64_t workingSetBytes = 0;                // median-run peak
};
struct BenchmarkProfile {
    bool present = false;
    std::string profileHash;
    std::int64_t capturedAtUnixMs = 0;
    std::vector<ProfileScenario> scenarios;
    const ProfileScenario* find(const std::string& id) const;
};
BenchmarkProfile profileFromReport(const SuiteReport& r);

// ---- admission -----------------------------------------------------------------------------
struct FeatureDef {
    std::string id;                               // stable feature id, e.g. "camera", "av-record", "second-output"
    Tier minTier = Tier::Base;
    std::string requiredScenario;                 // benchmark scenario that evidences it ("" = none needed)
    double cpuCostPct = 0;                        // estimated additional average CPU (same units as the profile)
    std::uint64_t memoryBytes = 0;                // estimated additional working set
    std::vector<std::string> requiredDevices;     // device ids that must be present
    bool needsEncoder = false;
};
struct AdmissionOverride { std::string featureId, reason; };   // documented operator override; reason required

enum class AdmissionState : std::uint8_t { Admitted, AdmittedByOverride, Rejected, Untested };
const char* admissionStateName(AdmissionState s);

struct AdmissionInput {
    Tier tier = Tier::Base;
    std::string currentProfileHash;               // hash of the CURRENT environment (compare with the profile)
    std::int64_t nowUnixMs = 0;
    std::int64_t maxProfileAgeMs = 30LL * 24 * 3600 * 1000;
    const BenchmarkProfile* profile = nullptr;
    std::vector<std::string> availableDevices;
    bool encoderAvailable = false;
    double cpuBudgetPct = BaseGateLimits::kAvgCpuPct;
    double cpuMarginPct = 5.0;                    // safety margin kept free
    std::uint64_t memoryBudgetBytes = BaseGateLimits::kSteadyBytes;
    std::vector<FeatureDef> requested;            // evaluated in this order; earlier admissions consume headroom
    std::vector<AdmissionOverride> overrides;
    bool qualityLocked = false;
    bool overloaded = false;                      // adaptation controller says overload warning is active
    int adaptationStep = 0;
};

struct FeatureDecision {
    std::string id;
    AdmissionState state = AdmissionState::Rejected;
    std::vector<std::string> reasons;
    bool overridable = false;                     // soft rejection that a documented override may lift
};
struct AdmissionResult {
    std::vector<FeatureDecision> decisions;
    double cpuHeadroomPct = 0;                    // before any feature was admitted
    std::uint64_t memoryHeadroomBytes = 0;
    bool baselineKnown = false;
    CapabilityStatus status;                      // IF-09
};
AdmissionResult admitFeatures(const AdmissionInput& in);

// ---- soak (REQ-X06) -------------------------------------------------------------------------
struct SoakSample {
    Ns t = 0;                                     // monotonic
    std::uint64_t workingSetBytes = 0, privateCommitBytes = 0;
    double maxQueueOccupancy = 0;
    bool blackoutActive = false, blackoutRequested = false;
    std::uint64_t captureOverruns = 0;            // cumulative
    std::uint64_t recordingFramesLost = 0, recordingLossDeclared = 0;   // cumulative; lost - declared = undeclared
};
struct SoakConfig {
    Ns duration = 4 * 3600 * kNsPerSec;
    double maxGrowthFraction = 0.05;              // tunable: allowed growth of mean working set / commit, early vs late
    std::uint64_t maxGrowthBytes = 8 * kMiB;      // growth below this is never a failure
    Ns stuckBlackoutNs = 10 * kNsPerSec;          // blackout active but not requested for longer than this
    std::uint64_t peakCapBytes = BaseGateLimits::kPeakBytes;
    std::size_t maxSamples = 1u << 16;
};
struct SoakReport {
    Verdict verdict = Verdict::Incomplete;
    std::vector<GateResult> checks;
    bool truncated = false;                       // more samples than maxSamples were offered
};
class SoakMonitor {
public:
    explicit SoakMonitor(const SoakConfig& c = {}) : cfg_(c) {}
    void add(const SoakSample& s);
    void noteCrash() { ++crashes_; }
    SoakReport evaluate() const;
    std::size_t samples() const { return s_.size(); }
private:
    SoakConfig cfg_;
    std::vector<SoakSample> s_;
    unsigned crashes_ = 0;
    bool truncated_ = false;
};

}  // namespace ss::perf
