#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "ss/perf/benchmark.hpp"

namespace ss::app {
ss::Result<ss::perf::SuiteReport> runSoftwareBenchmark(ss::perf::IBenchTime& time, ss::perf::RunnerConfig config,
                                                     const std::atomic<bool>& cancel);
struct BenchmarkView {
    bool running = false;
    std::uint64_t generation = 0;
    std::string note = "No benchmark result. This machine is unqualified.";
    std::string profileHash;
};
class SoftwareBenchmark final {
public:
    ~SoftwareBenchmark() { stop(); }
    bool start(bool quick = true);
    void cancel() { cancel_.store(true); }
    void stop();
    BenchmarkView view() const;
    std::string report() const;
private:
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    BenchmarkView view_;
    std::string report_;
};
}  // namespace ss::app
