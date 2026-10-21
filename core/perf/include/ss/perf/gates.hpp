// StageSignal perf (MOD-14): BASE gate evaluator (REQ-P06 and the section 7 profile qualification list).
// The evaluator is pure: it reads a MetricSet and produces pass/fail/unavailable per gate with the measured
// value and the limit. A missing counter is Unavailable (never Pass), so a verdict is Pass only when every
// evaluated gate was measured and met.
#pragma once
#include <string>
#include <vector>
#include "ss/perf/limits.hpp"
#include "ss/perf/metrics.hpp"

namespace ss::perf {

enum class GateState : std::uint8_t { Pass, Fail, Unavailable };
enum class Verdict : std::uint8_t { Pass, Fail, Incomplete };   // Incomplete: no Fail but at least one Unavailable

const char* gateStateName(GateState s);
const char* verdictName(Verdict v);

struct GateThresholds {   // defaults == BASE (section 7)
    double minRateFraction = BaseGateLimits::kMinRateFraction;
    double renderP95OfPeriod = BaseGateLimits::kRenderP95OfPeriod;
    double maxMissedFraction = BaseGateLimits::kMaxMissedFraction;
    double minWindowShare = BaseGateLimits::kMinWindowShare;
    int baseTargetFps = BaseGateLimits::kBaseTargetFps;
    double intervalP95Ms = BaseGateLimits::kIntervalP95Ms, intervalP99Ms = BaseGateLimits::kIntervalP99Ms,
           stallMs = BaseGateLimits::kStallMs;
    double uiAckP95Ms = BaseGateLimits::kUiAckP95Ms, sceneP95Ms = BaseGateLimits::kSceneP95Ms,
           onsetP95Ms = BaseGateLimits::kOnsetP95Ms;
    std::uint64_t steadyBytes = BaseGateLimits::kSteadyBytes, peakBytes = BaseGateLimits::kPeakBytes,
                  hostBytes = BaseGateLimits::kHostBytes, minFreeBytes = BaseGateLimits::kMinFreeBytes;
    double avgCpuPct = BaseGateLimits::kAvgCpuPct, p95CpuPct = BaseGateLimits::kP95CpuPct;
};

struct GateInput {
    const MetricSet* metrics = nullptr;
    double presentationRateHz = 25.0;          // configured target presentation rate for the run
    bool sceneFpsLoweredByShedding = false;    // true when adaptation reduced the scene rate during the run
    bool gateLatency = true;                   // UI ack / scene / onset response gates
    bool gateCaptureOverruns = false;          // zero capture overruns (BEN-06 and the 4 h soak)
};

struct GateResult {
    std::string id;           // stable, e.g. "G-FPS-RATE"
    std::string description;
    GateState state = GateState::Unavailable;
    double measured = 0;      // meaningful unless state == Unavailable
    double limit = 0;
    bool upperBound = true;   // true: pass when measured <= limit; false: pass when measured >= limit
    std::string unit;
    std::string req;          // requirement text id, e.g. "REQ-P06"
};

struct GateReport {
    std::vector<GateResult> gates;
    Verdict verdict = Verdict::Incomplete;
    int passed = 0, failed = 0, unavailable = 0;
};

GateReport evaluateBaseGates(const GateThresholds& t, const GateInput& in);

}  // namespace ss::perf
