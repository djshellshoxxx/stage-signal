// StageSignal perf (MOD-14): benchmark scenarios BEN-01..06, deterministic report schema and the runner
// (REQ-P01, REQ-P02, section 7).
// The runner is driven by an injected time source and an IBenchWorkload, so it is fully testable with fake
// workloads and fake time. REQ-P02: a workload declares its sinks up front and the runner refuses to start
// unless every sink is a mock, a disposable local file or a local discard receiver. Live outputs, the real
// recorder, lighting adapters and any remote broadcast endpoint are refused before prepare() is called.
#pragma once
#include <atomic>
#include <string>
#include <utility>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/perf/gates.hpp"
#include "ss/perf/metrics.hpp"

namespace ss::perf {

// ---- sinks (REQ-P02) -----------------------------------------------------------------------
enum class SinkKind : std::uint8_t {
    None, Mock, DisposableFile, LocalDiscard,            // allowed in benchmarks
    LiveOutput, LiveRecording, LiveLighting, Broadcast   // never allowed in benchmarks
};
struct SinkDecl {
    SinkKind kind = SinkKind::None;
    std::string label;      // short, sanitized
    std::string endpoint;   // network sinks only; must be loopback/localhost (or empty for non-network)
};
const char* sinkKindName(SinkKind k);
bool isLoopbackEndpoint(const std::string& endpoint);   // "", "localhost", "127.x.x.x", "::1", "[::1]", with optional :port
// Allowed = a benign kind AND (for LocalDiscard) a loopback/empty endpoint.
bool sinkAllowed(const SinkDecl& s);

// ---- scenarios -----------------------------------------------------------------------------
struct ScenarioDescriptor {
    std::string id;                          // "BEN-01"
    std::string name;
    std::string workload;                    // spec wording
    std::vector<std::string> evidence;       // recorded evidence (spec wording)
    bool needsCamera = false, needsEncoder = false, needsBroadcastEncoder = false, multiOutput = false;
    bool gateLatency = false;                // evaluate UI/scene/onset response gates
    bool gateCaptureOverruns = false;        // evaluate zero capture overruns
    std::vector<SinkKind> allowedSinks;      // benign sinks this scenario may use
};
const std::vector<ScenarioDescriptor>& scenarios();       // BEN-01..BEN-06 in order
const ScenarioDescriptor* findScenario(const std::string& id);

// ---- workload and time ---------------------------------------------------------------------
class IBenchWorkload {
public:
    virtual ~IBenchWorkload() = default;
    virtual std::vector<SinkDecl> sinks() const = 0;
    virtual double presentationRateHz() const = 0;
    virtual Status prepare(const ScenarioDescriptor& scenario, std::uint64_t seed) = 0;
    // Advance the workload to `now` (monotonic) and report measurements into `rec`. Called every tick.
    virtual void step(Ns now, RunRecorder& rec) = 0;
    virtual void finish() {}
    // True if adaptive shedding lowered the scene render rate during the benchmark (fails G-TARGET-FPS).
    virtual bool sceneRateLowered() const { return false; }
    virtual int adaptationStep() const { return 0; }
};

class IBenchTime : public IClock {
public:
    virtual void waitUntil(Ns mono) = 0;     // real: sleep; fake: jump
};

// Deterministic time source for tests and for offline simulation.
class FakeBenchTime final : public IBenchTime {
public:
    explicit FakeBenchTime(Ns start = 0) : now_(start) {}
    Ns nowMonotonic() const override { return now_; }
    Ns wallUnixNs() const override { return 1'700'000'000LL * kNsPerSec + now_; }
    void waitUntil(Ns mono) override { if (mono > now_) now_ = mono; }
private:
    Ns now_;
};

// ---- environment and report ----------------------------------------------------------------
struct MonitorInfo { int width = 0, height = 0, rotationDeg = 0; bool primary = false; };
struct SizeInfo { int width = 0, height = 0; };
struct BenchEnvironment {
    std::string appHash;
    std::vector<std::pair<std::string, std::string>> dependencyHashes;   // name -> hash
    std::string os, powerPlan, cpu, gpu, gpuDriver;
    unsigned logicalProcessors = 0;
    bool gpuTimingAvailable = false;
    std::vector<MonitorInfo> monitors;
    std::vector<SizeInfo> compositions;
    std::uint32_t captureRateHz = 0, captureChannels = 0;
    std::string captureFormat;               // e.g. "float32"
    std::vector<std::string> mediaHashes;
    std::string encoder;                     // empty = none
    std::uint32_t bitrateKbps = 0;
};

struct SkippedTest { std::string scenarioId, reason; };

struct ScenarioReport {
    std::string scenarioId, scenarioName;
    std::uint64_t seed = 0;
    double presentationRateHz = 0;
    double warmupSec = 0;
    std::uint64_t warmupFrames = 0;          // warm-up/loading is recorded separately and never gated
    std::vector<MetricSet> runs;
    MetricSet median, worst;
    GateReport gatesWorst, gatesMedian;      // verdict = gatesWorst.verdict
    int adaptationStepAtEnd = 0;
    bool sceneRateLowered = false;
};

struct SuiteReport {
    std::string schema = "stagesignal.benchmark/1";
    BenchEnvironment env;
    std::int64_t capturedAtUnixMs = 0;       // set by the caller; 0 when unknown
    std::vector<ScenarioReport> scenarios;
    std::vector<SkippedTest> skipped;
    std::string profileHash() const;         // sha256 hex of the canonical environment JSON
};

// Deterministic JSON (sorted keys, values rounded to 4 decimals). Same SuiteReport -> byte-identical text.
std::string toJson(const SuiteReport& r, int indent = 2);
std::string environmentJson(const BenchEnvironment& env);

// ---- runner --------------------------------------------------------------------------------
struct RunnerConfig {
    Ns warmupNs = kWarmupNs;
    Ns runNs = kRunNs;
    int runs = kRunsPerScenario;
    Ns tickNs = 5 * kNsPerMs;
    GateThresholds thresholds;
    const std::atomic<bool>* cancel = nullptr;   // operator abort
};

struct SuiteEntry {
    std::string scenarioId;
    IBenchWorkload* workload = nullptr;     // null => skipped
    std::string skipReason;                  // used when workload is null ("no camera attached")
};

class BenchmarkRunner {
public:
    BenchmarkRunner(IBenchTime& time, const RunnerConfig& cfg) : time_(time), cfg_(cfg) {}
    // Validates sinks (REQ-P02) and config first; nothing is started on refusal.
    Status validate(const ScenarioDescriptor& s, const IBenchWorkload& w) const;
    Result<ScenarioReport> run(const ScenarioDescriptor& s, IBenchWorkload& w, std::uint64_t seed);
    // Runs every entry; unavailable scenarios are recorded as skipped, a refused sink aborts that entry
    // as skipped with the refusal text and the suite continues.
    Result<SuiteReport> runSuite(const BenchEnvironment& env, const std::vector<SuiteEntry>& entries, std::uint64_t seed);
private:
    IBenchTime& time_;
    RunnerConfig cfg_;
};

// Aggregation helper (exposed for tests): median = lower median, worst direction-aware per metric.
void aggregateRuns(const std::vector<MetricSet>& runs, MetricSet* median, MetricSet* worst);

}  // namespace ss::perf
