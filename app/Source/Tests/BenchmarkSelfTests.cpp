#include "App/Benchmark/SoftwareBenchmark.h"
#include "Tests/SelfTest.h"
namespace ss::app {
void registerBenchmarkSelfTests(SelfTestRegistry& registry) {
    registry.add("benchmark: real software workloads and honest qualification", [] {
        ss::perf::FakeBenchTime time;
        ss::perf::RunnerConfig config;
        config.warmupNs = 0; config.runNs = msToNs(100); config.runs = 1;
        std::atomic<bool> cancel{false};
        auto result = runSoftwareBenchmark(time, config, cancel);
        if (!result.ok()) return SelfTestResult{false, result.error().detail};
        if (result.value().scenarios.size() != 2 || result.value().skipped.size() != 4)
            return SelfTestResult{false, "required available/skipped workload accounting changed"};
        for (const auto& scenario : result.value().scenarios) {
            if (scenario.runs.empty() || scenario.runs.front().value("frames.presented") < 1) return SelfTestResult{false, "software renderer did not execute"};
            if (scenario.gatesMedian.verdict == ss::perf::Verdict::Pass)
                return SelfTestResult{false, "missing hardware/latency/counter evidence falsely qualified"};
        }
        cancel.store(true);
        if (runSoftwareBenchmark(time, config, cancel).ok()) return SelfTestResult{false, "cancel ignored"};
        return SelfTestResult{true, "BEN-01/BEN-02 execute software scene/audio-band/text work; other scenarios skipped, no qualification invented"};
    });
}
}
