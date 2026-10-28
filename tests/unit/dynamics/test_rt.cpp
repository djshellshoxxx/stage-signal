// T-RT: zero heap allocations on the process path. This TU replaces global operator new/delete with counting versions
// (the replacement applies to the whole test executable, which is what we want).
#include <atomic>
#include <cstdlib>
#include <new>
#include "doctest/doctest.h"
#include "dsp_helpers.hpp"
#include "ss/dynamics/sync.hpp"

namespace {
std::atomic<long> g_news{0};
std::atomic<bool> g_counting{false};
int* volatile g_sink = nullptr;
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
struct CountScope {
    CountScope() { g_news = 0; g_counting = true; }
    ~CountScope() { g_counting = false; }
    long count() const { return g_news.load(); }
};
}  // namespace

TEST_CASE("REQ-P05: allocation counter works (sanity)") {
    CountScope s;
    int* p = new int[16];
    g_sink = p;   // keeps the compiler from eliding the allocation
    CHECK(s.count() == 1);
    delete[] p;
}

TEST_CASE("REQ-P05: DynamicsProcessor::process performs zero heap allocations") {
    using namespace dyntest;
    DynamicsConfig cfg;
    DynamicsProcessor proc;
    proc.setParams(DynamicsParams{});
    REQUIRE(proc.prepare(cfg).ok());
    std::vector<float> in = sineStereo(512, 1000.0, 1.2, 48000.0), out(1024);
    // warm up: adopt the engine (pointer swap only) outside the counted region is NOT required, so count from the start
    ss::Rng rng(3);
    {
        CountScope scope;
        for (int i = 0; i < 400; ++i) {
            DynamicsParams p;
            p.trimDb = rng.range(-24, 12); p.thresholdDb = rng.range(-36, 0); p.ratio = rng.range(1, 10);
            p.kneeDb = rng.range(0, 12); p.makeupDb = rng.range(0, 12); p.ceilingDb = rng.range(-12, -0.5f);
            p.attackMs = rng.range(0.5f, 100); p.releaseMs = rng.range(20, 2000); p.limiterReleaseMs = rng.range(20, 500);
            p.bypass = (i / 20) % 2 == 1;
            proc.setParams(p);
            proc.process(in.data(), out.data(), 512);
            proc.process(in.data(), out.data(), 1);
            proc.process(in.data(), out.data(), 0);
            (void)proc.telemetry();
            (void)proc.params();
            proc.resetHolds();
        }
        CHECK(scope.count() == 0);
    }
    // adopting a freshly prepared engine inside process() is a pointer swap; the allocation happened in prepare()
    REQUIRE(proc.prepare(cfg).ok());
    {
        CountScope scope;
        proc.process(in.data(), out.data(), 512);
        CHECK(scope.count() == 0);
    }
    CHECK(proc.activeInfo().processingDelaySamples == 240);
}

TEST_CASE("REQ-P05: sync hot paths (LinearResampler, ClockMap prediction) do not allocate after construction") {
    ss::dynamics::LinearResampler rs(2);
    rs.setRatio(1.0002);
    ss::dynamics::ClockMap map;
    std::vector<float> in(4096 * 2, 0.25f), out(4200 * 2);
    for (int i = 0; i < 40; ++i) map.addPacket(480ull * static_cast<unsigned>(i + 1), 10'000'000LL * (i + 1) + 500'000'000LL * (i / 1));
    CountScope scope;
    for (int i = 0; i < 50; ++i) rs.process(in.data(), 4096, out.data(), 4200);
    for (int i = 0; i < 50; ++i) (void)map.hostNsForSample(480ull * 100);
    for (int i = 0; i < 100; ++i) map.addPacket(480ull * (1000 + static_cast<unsigned>(i)), 2'000'000'000LL + 10'000'000LL * i + 1'000'000'000LL * i);
    (void)map.estimate();
    CHECK(scope.count() == 0);
}

TEST_CASE("REQ-P05: the true-peak limiter does not allocate in process() or on parameter changes") {
    using namespace dyntest;
    DynamicsConfig cfg;
    cfg.truePeakLimiter = true;
    DynamicsProcessor proc;
    proc.setParams(DynamicsParams{});
    REQUIRE(proc.prepare(cfg).ok());
    std::vector<float> in = sineStereo(512, 15000.0, 1.6, 48000.0, 0.7), out(1024);
    ss::Rng rng(11);
    {
        CountScope scope;
        for (int i = 0; i < 300; ++i) {
            DynamicsParams p;
            p.thresholdDb = rng.range(-36, 0); p.ratio = rng.range(1, 10); p.ceilingDb = rng.range(-12, -0.5f);
            p.limiterReleaseMs = rng.range(20, 500); p.bypass = (i / 25) % 2 == 1;
            proc.setParams(p);
            proc.process(in.data(), out.data(), 512);
            proc.process(in.data(), out.data(), 1);
            (void)proc.telemetry();
        }
        CHECK(scope.count() == 0);
    }
    long processAllocs = 0;
    {
        CountScope scope;
        for (int i = 0; i < 100; ++i) proc.process(in.data(), out.data(), 512);
        processAllocs = scope.count();
    }
    CHECK(processAllocs == 0);
}
