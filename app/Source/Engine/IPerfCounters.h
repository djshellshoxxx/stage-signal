// Contract: platform performance counters for the perf core (app-shell 3 PerfMonitor). Owner: engine-adapters-desktop.
// Time itself is ss::IClock (core/base). Counters are sampled at 1 Hz on the CONTROL thread.
#pragma once
#include <cstdint>

namespace ss::app {

struct PerfSample {
    double processCpuPct = 0.0;            // 0..100 normalised over logical cores
    std::uint64_t workingSetBytes = 0;
    unsigned logicalCores = 1;
    bool valid = false;
};

class IPerfCounters {
public:
    virtual ~IPerfCounters() = default;
    virtual PerfSample sample() = 0;       // CONTROL thread, <= 1 ms
};

class FixedPerfCounters final : public IPerfCounters {
public:
    PerfSample sample() override {
        PerfSample s; s.valid = true; s.logicalCores = 2; s.processCpuPct = 1.0; s.workingSetBytes = 64u << 20;
        return s;
    }
};

}  // namespace ss::app
