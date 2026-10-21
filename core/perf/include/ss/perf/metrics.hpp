// StageSignal perf (MOD-14): metric collectors (REQ-P01, section 7 "Recorded evidence").
// Pure data + arithmetic. No clocks, no OS counters: callers pass timestamps and raw samples in, so the
// same collector is used by the live app (fed by platform counters) and by fake workloads in tests.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"

namespace ss::perf {

// One named measurement. `available == false` means the counter was missing; value is then 0 and must
// never be read as a real zero (IF-09: "missing counters explicitly unavailable").
struct Metric {
    std::string name;
    double value = 0.0;
    bool available = false;
};

// Ordered, deterministic set of metrics (canonical order = insertion order).
class MetricSet {
public:
    void set(const std::string& name, double value) { put(name, value, true); }
    void setUnavailable(const std::string& name) { put(name, 0.0, false); }
    const Metric* find(const std::string& name) const;
    bool has(const std::string& name) const { return find(name) != nullptr; }
    bool available(const std::string& name) const { const Metric* m = find(name); return m && m->available; }
    double value(const std::string& name, double fallback = 0.0) const { const Metric* m = find(name); return m && m->available ? m->value : fallback; }
    const std::vector<Metric>& all() const { return items_; }
private:
    void put(const std::string& name, double value, bool avail);
    std::vector<Metric> items_;
};

// True when a larger value of this metric is better (FPS, free memory); false for latency/CPU/etc.
// Used to pick the "worst" of three runs.
bool metricHigherIsBetter(const std::string& name);

struct Percentiles {
    bool available = false;
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
    std::size_t count = 0;
};

// Bounded sample ring with nearest-rank percentiles. add() never allocates; percentiles() copies into a
// scratch buffer that was allocated at construction, so a control thread may query it at any rate.
class SampleWindow {
public:
    explicit SampleWindow(std::size_t capacity = 4096);
    void add(double v);
    void clear();
    std::size_t size() const { return count_; }
    std::size_t capacity() const { return buf_.size(); }
    Percentiles percentiles() const;
private:
    std::vector<double> buf_;
    mutable std::vector<double> scratch_;
    std::size_t head_ = 0, count_ = 0;
};

enum class MissKind : std::uint8_t { Render, Presentation, Decoder, Encoder };

struct CpuSample {            // one sample per second
    bool available = false;
    double systemPct = 0;     // 0..100 averaged across logical processors
    double busiestCorePct = 0;
    double processPct = 0;    // process CPU time / (wall * logical processors) * 100
};

// Helper that normalises raw per-core busy percentages and process CPU time deltas (platform adapters call this).
CpuSample normaliseCpu(const std::vector<double>& perCoreBusyPct, Ns processCpuNsDelta, Ns wallNsDelta,
                       unsigned logicalProcessors);

struct MemSample {
    bool workingSetAvailable = false, commitAvailable = false, hostAvailable = false, freeAvailable = false;
    std::uint64_t workingSetBytes = 0;     // all StageSignal processes
    std::uint64_t privateCommitBytes = 0;
    std::uint64_t hostWorkingSetBytes = 0; // host process only
    std::uint64_t availablePhysBytes = 0;  // system-wide available physical memory
};

// Collects everything one benchmark run needs. Create one per phase (warm-up, each run).
class RunRecorder {
public:
    // presentationRateHz: configured presentation rate; cap: ring size for the frame percentile windows.
    explicit RunRecorder(double presentationRateHz, std::size_t cap = 1u << 14);

    void begin(Ns startNs);
    // A frame completed and reached presentation at `presentedAt` after `renderNs` of render work.
    void frame(Ns presentedAt, Ns renderNs);
    // One presentation deadline passed; `met` false counts a missed presentation deadline.
    void presentationDeadline(bool met);
    void miss(MissKind kind, std::uint64_t n = 1);
    void queueOccupancy(const std::string& name, double occupancy01);
    void cpu(const CpuSample& s);
    void mem(const MemSample& s);
    void uiAck(Ns latency);
    void sceneResponse(Ns latency);
    void audioToVisualOnset(Ns latency);
    void captureOverrun(std::uint64_t n = 1) { overruns_ += n; overrunsReported_ = true; }
    void declareCaptureOverrunsMeasured() { overrunsReported_ = true; }

    // Close the run at `endNs` and produce the canonical metric set.
    MetricSet finish(Ns endNs) const;

    double rateHz() const { return rateHz_; }
    int minFramesPerWindow() const { return minFrames_; }
    std::uint64_t framesPresented() const { return frames_; }

private:
    struct Q { std::string name; double max = 0, sum = 0; std::uint64_t n = 0; };
    double rateHz_;
    int minFrames_;
    Ns start_ = 0, last_ = -1;
    std::uint64_t frames_ = 0;
    // fps windows (closed lazily as frames arrive; finish() closes the remainder on a copy)
    std::int64_t curIdx_ = 0;
    std::uint64_t curCount_ = 0, winTotal_ = 0, winMeeting_ = 0;
    SampleWindow interval_, render_, ui_, scene_, onset_, cpuProc_, cpuSys_, cpuBusy_, ws_, commit_;
    std::uint64_t miss_[4] = {0, 0, 0, 0}, due_ = 0, missedPres_ = 0;
    bool deadlinesRecorded_ = false;
    std::vector<Q> queues_;
    double cpuProcSum_ = 0, cpuSysSum_ = 0, cpuBusySum_ = 0;
    std::uint64_t cpuN_ = 0;
    std::uint64_t wsPeak_ = 0, commitPeak_ = 0, hostPeak_ = 0, freeMin_ = ~0ull;
    std::uint64_t wsN_ = 0, commitN_ = 0, hostN_ = 0, freeN_ = 0;
    std::uint64_t overruns_ = 0;
    bool overrunsReported_ = false;
};

}  // namespace ss::perf
