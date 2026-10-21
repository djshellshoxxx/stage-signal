// T-RT: zero allocation in process() after prepare(), plus fuzz/robustness (NaN/Inf/huge, odd blocks, flags).
// This translation unit replaces the global operator new/delete of the test executable with counting versions.
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include "doctest/doctest.h"
#include "test_util.hpp"
#include "ss/audio/analyzer.hpp"

namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_armed{false};
}

void* operator new(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace ss;
using namespace ss::audio;

namespace {
// ~12 s of "music": kick + snare pattern, chord pad, noise bed. Stereo interleaved.
std::vector<float> musicLike(std::uint32_t rate, double sec) {
    Rng rng(77);
    std::vector<float> m(static_cast<std::size_t>(sec * rate), 0.f);
    int i = 0;
    for (double t = 0.0; t + 0.3 < sec; t += 0.25, ++i) sst::addInPlace(m, sst::hit(i % 4 == 2 ? 1 : (i % 2 == 0 ? 2 : 3), 0.5, rate, rng), static_cast<std::size_t>(t * rate));
    auto pad = sst::progression(0, false, sec, rate);
    for (std::size_t k = 0; k < m.size(); ++k) m[k] = std::clamp(m[k] + pad[k], -1.f, 1.f);
    return sst::toChannels(m, 2);
}
}

TEST_CASE("T-RT: process(), snapshot() and the copy helpers allocate nothing after prepare()") {
    const std::uint32_t rate = 48000;
    const auto sig = musicLike(rate, 12.0);
    Analyzer a;
    REQUIRE(a.prepare(rate, 2).ok());
    std::vector<float> junk(4096 * 2, 0.5f);                         // pre-built, so building it does not count
    std::vector<float> nanBlock(1024 * 2, std::numeric_limits<float>::quiet_NaN());
    OnsetEvent ev[64]; float sp[32], wf[256];
    std::size_t blocks[] = {1, 7, 64, 480, 1023, 4096};

    g_allocs = 0;
    g_armed = true;
    std::uint64_t idx = 0;
    std::size_t pos = 0, bi = 0, frames = sig.size() / 2;
    while (pos < frames) {
        const std::size_t n = std::min(blocks[bi % 6], frames - pos);
        ++bi;
        AudioBlockView v; v.interleaved = sig.data() + pos * 2; v.frames = static_cast<std::uint32_t>(n);
        v.channels = 2; v.rateHz = rate; v.firstSampleIndex = idx;
        v.flags = (bi % 97 == 0) ? kAudioDiscontinuity : 0u;
        a.process(v);
        idx += n; pos += n;
        if (bi % 5 == 0) { (void)a.snapshot(); a.copySpectrum(sp); a.copyWaveform(wf); (void)a.drainOnsets(ev, 64); }
        if (bi % 301 == 0) {
            AudioBlockView nb; nb.interleaved = nanBlock.data(); nb.frames = 1024; nb.channels = 2; nb.rateHz = rate;
            a.process(nb);
            AudioBlockView mm; mm.interleaved = junk.data(); mm.frames = 100; mm.channels = 1; mm.rateHz = rate;   // mismatch
            a.process(mm);
        }
    }
    g_armed = false;
    CHECK(g_allocs.load() == 0);
    CHECK(a.stats().samples > 500000);
    CHECK(a.stats().nonFiniteSamples > 0);
    CHECK(a.stats().mismatchedBlocks > 0);
    CHECK(a.stats().onsets > 10);
}

TEST_CASE("T-RT: the counting allocator actually counts (guards against a vacuous test)") {
    g_allocs = 0; g_armed = true;
    static int* volatile sink;
    sink = new int[16];
    g_armed = false;
    delete[] sink;
    CHECK(g_allocs.load() >= 1);
}

TEST_CASE("T-RT: fuzz with NaN/Inf/huge values, random block sizes, flags and mismatched formats keeps all outputs sane") {
    const float inf = std::numeric_limits<float>::infinity();
    const float big[] = {std::numeric_limits<float>::quiet_NaN(), inf, -inf, 1e30f, -1e30f, std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::denorm_min(), 1e-30f, 0.f, -0.f, 64.5f, -1000.f, 1.0f, -1.0f};
    for (std::uint32_t ch : {1u, 2u}) {
        for (std::uint32_t rate : {44100u, 48000u}) {
            Analyzer a; REQUIRE(a.prepare(rate, ch).ok());
            Rng rng(1000 + ch + rate);
            std::vector<float> buf(5000 * 2);
            std::uint64_t idx = 0;
            for (int iter = 0; iter < 300; ++iter) {
                const std::uint32_t frames = (iter % 17 == 0) ? 0u : 1u + rng.below(rng.below(4) == 0 ? 4096 : 600);
                const int mode = static_cast<int>(rng.below(6));
                for (std::size_t i = 0; i < std::size_t(frames) * ch; ++i) {
                    switch (mode) {
                        case 0: buf[i] = rng.range(-1.f, 1.f); break;
                        case 1: buf[i] = big[rng.below(sizeof(big) / sizeof(big[0]))]; break;
                        case 2: buf[i] = rng.below(8) == 0 ? big[rng.below(sizeof(big) / sizeof(big[0]))] : rng.range(-0.3f, 0.3f); break;
                        case 3: buf[i] = static_cast<float>(0.9 * std::sin(0.05 * double(idx + i))); break;
                        case 4: buf[i] = 0.f; break;
                        default: buf[i] = rng.range(-4.f, 4.f); break;
                    }
                }
                AudioBlockView v; v.interleaved = buf.data(); v.frames = frames; v.channels = ch; v.rateHz = rate;
                v.firstSampleIndex = idx + (rng.below(10) == 0 ? rng.below(100000) : 0);
                v.flags = (rng.below(8) == 0 ? kAudioDiscontinuity : 0u) | (rng.below(12) == 0 ? kAudioSilent : 0u) |
                          (rng.below(5) == 0 ? kAudioTimestampValid : 0u);
                v.captureNs = static_cast<Ns>(idx) * 20833;
                if (rng.below(25) == 0) { v.rateHz = rate == 48000 ? 44100 : 48000; }
                if (rng.below(40) == 0) { v.channels = ch == 1 ? 2 : 1; }
                if (rng.below(50) == 0) v.interleaved = nullptr;
                a.process(v);
                idx += frames;
                const FeatureSnapshot s = a.snapshot();
                for (int c = 0; c < 2; ++c) {
                    REQUIRE(std::isfinite(s.rmsDb[c])); REQUIRE(s.rmsDb[c] >= -120.f); REQUIRE(s.rmsDb[c] <= 40.f);
                    REQUIRE(std::isfinite(s.peakDb[c])); REQUIRE(s.peakDb[c] >= -120.f); REQUIRE(s.peakDb[c] <= 40.f);
                }
                for (float b : s.band) { REQUIRE(std::isfinite(b)); REQUIRE(b >= 0.f); }
                REQUIRE(s.onsetStrength >= 0.f); REQUIRE(s.onsetStrength <= 1.f);
                REQUIRE(s.energyArc >= 0.f); REQUIRE(s.energyArc <= 1.f);
                REQUIRE(s.arcPhase <= 2);
                REQUIRE(std::isfinite(s.tempo.bpm));
                REQUIRE((s.tempo.valid() ? (s.tempo.bpm >= 38.0 && s.tempo.bpm <= 250.0) : s.tempo.bpm == 0.0));
                REQUIRE(s.tempo.confidence >= 0.f); REQUIRE(s.tempo.confidence <= 1.f);
                REQUIRE(s.key.tonic >= -1); REQUIRE(s.key.tonic <= 11);
                REQUIRE(s.key.confidence >= 0.f); REQUIRE(s.key.confidence <= 1.f);
                REQUIRE(s.tempo.ageNs >= 0); REQUIRE(s.key.ageNs >= 0);
                float sp[32], wf[256];
                a.copySpectrum(sp); a.copyWaveform(wf);
                for (float x : sp) { REQUIRE(std::isfinite(x)); REQUIRE(x >= 0.f); REQUIRE(x <= 1.f); }
                for (float x : wf) { REQUIRE(std::isfinite(x)); REQUIRE(std::fabs(x) <= 1.f); }
            }
            CHECK(a.stats().nonFiniteSamples > 0);
            CHECK(a.stats().clampedSamples > 0);
            CHECK(a.stats().mismatchedBlocks > 0);
            CHECK(a.stats().discontinuities > 0);
            // recovery: a clean tone afterwards is measured correctly
            a.reset();
            sst::Feeder fd(a, ch, rate);
            fd.feed(sst::toChannels(sst::sine(1000, 0.5, 0.4, rate), static_cast<int>(ch)), 480);
            const FeatureSnapshot s = a.snapshot();
            CHECK(std::fabs(10 * std::log10(s.band[1]) - 20 * std::log10(0.5)) < 0.5);
            CHECK(a.stats().nonFiniteSamples == 0);
        }
    }
}

TEST_CASE("T-RT: unprepared and re-prepared analyzers are safe") {
    Analyzer a;
    float x[16] = {};
    AudioBlockView v; v.interleaved = x; v.frames = 8; v.channels = 2; v.rateHz = 48000;
    a.process(v);                                                    // not prepared: ignored
    CHECK(a.snapshot().silent);
    float sp[32], wf[256];
    a.copySpectrum(sp); a.copyWaveform(wf);
    a.copySpectrum(nullptr); a.copyWaveform(nullptr);
    REQUIRE(a.prepare(48000, 2).ok());
    a.process(v);
    REQUIRE(a.prepare(44100, 1).ok());                               // re-prepare with different format
    CHECK(a.rateHz() == 44100);
    CHECK(a.channels() == 1);
    CHECK(a.stats().samples == 0);
}
