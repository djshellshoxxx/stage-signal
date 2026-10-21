// StageSignal perf (MOD-14): spec section 7 thresholds and bounds in one place. Values are proposed
// engineering gates from the spec, not measured capabilities.
#pragma once
#include <cstddef>
#include <cstdint>
#include "ss/base/time.hpp"

namespace ss::perf {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

// Benchmark protocol (section 7).
constexpr Ns kWarmupNs = 15 * kNsPerSec;
constexpr Ns kRunNs = 60 * kNsPerSec;
constexpr int kRunsPerScenario = 3;

// Bounds (checked before allocation).
constexpr std::size_t kMaxWindowSamples = 1u << 16;     // percentile ring capacity ceiling
constexpr std::size_t kMaxNamedQueues = 8;
constexpr std::size_t kMaxReportStrings = 64;            // list entries accepted in a report environment
constexpr std::size_t kMaxStringBytes = 256;

// BASE gate thresholds (REQ-P06 + section 7).
struct BaseGateLimits {
    static constexpr double kMinRateFraction = 0.95;       // >= 95% of presentation rate
    static constexpr double kRenderP95OfPeriod = 0.80;     // p95 render <= 80% of period
    static constexpr double kMaxMissedFraction = 0.01;     // <= 1% missed presentation deadlines
    static constexpr double kMinWindowShare = 0.95;        // frames-per-window met in 95% of 1 s windows
    static constexpr int kBaseTargetFps = 25;
    static constexpr double kIntervalP95Ms = 50.0, kIntervalP99Ms = 100.0, kStallMs = 200.0;
    static constexpr double kUiAckP95Ms = 100.0, kSceneP95Ms = 150.0, kOnsetP95Ms = 150.0;
    static constexpr std::uint64_t kSteadyBytes = 96 * kMiB, kPeakBytes = 128 * kMiB, kHostBytes = 64 * kMiB;
    static constexpr double kAvgCpuPct = 65.0, kP95CpuPct = 80.0;
    static constexpr std::uint64_t kMinFreeBytes = 32 * kMiB;
};

// Adaptive-quality defaults (section 7). All tunable through AdaptationConfig with validated ranges.
struct AdaptDefaults {
    static constexpr double kCpuTriggerPct = 85.0;
    static constexpr Ns kCpuTriggerNs = 5 * kNsPerSec;
    static constexpr double kMissTriggerFraction = 0.05;
    static constexpr Ns kMissWindowNs = 5 * kNsPerSec;
    static constexpr double kQueueTrigger = 0.75;
    static constexpr Ns kQueueTriggerNs = 2 * kNsPerSec;
    static constexpr Ns kShedIntervalNs = 5 * kNsPerSec;
    static constexpr double kCpuCalmPct = 65.0;
    static constexpr double kMissCalmFraction = 0.01;
    static constexpr double kQueueCalm = 0.25;
    static constexpr Ns kCalmNs = 30 * kNsPerSec;
    static constexpr Ns kRestoreIntervalNs = 30 * kNsPerSec;
};

}  // namespace ss::perf
