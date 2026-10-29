// REQ-R02 / REQ-A10 / T-DSP: true-peak lookahead limiter. Validated on SYNTHETIC adversarial signals against an independent
// reference meter (16x, 129-tap Kaiser sinc evaluated directly in double precision, written here and sharing no code with the
// production detector). No music corpus was used: see docs/specs/dynamics.md "Status" for what is and is not claimed.
#include <algorithm>
#include <cmath>
#include <vector>
#include "doctest/doctest.h"
#include "dsp_helpers.hpp"
#include "ss/base/rng.hpp"
#include "ss/dynamics/processor.hpp"

using namespace dyntest;

namespace {
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 100; ++k) { const double h = x / (2.0 * k); term *= h * h; sum += term; if (term < 1e-18 * sum) break; }
    return sum;
}

// Independent reference: true peak (linear) of one channel of an interleaved buffer between frames [from, to).
double referenceTruePeak(const std::vector<float>& v, std::size_t ch, std::size_t chans, std::size_t from, std::size_t to) {
    constexpr int kHalf = 64, kSub = 16;
    constexpr double beta = 9.0;
    const std::size_t frames = v.size() / chans;
    std::vector<double> kernel(static_cast<std::size_t>(kSub) * (2 * kHalf + 1));
    for (int s = 0; s < kSub; ++s)
        for (int k = -kHalf; k <= kHalf; ++k) {
            const double t = static_cast<double>(s) / kSub - k;        // distance from the sample at index n+k
            const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double r = t / (kHalf + 1);
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / besselI0(beta);
            kernel[static_cast<std::size_t>(s) * (2 * kHalf + 1) + static_cast<std::size_t>(k + kHalf)] = sinc * w;
        }
    double best = 0.0;
    for (std::size_t n = std::max<std::size_t>(from, kHalf); n + kHalf < std::min(to, frames); ++n)
        for (int s = 0; s < kSub; ++s) {
            double acc = 0.0;
            for (int k = -kHalf; k <= kHalf; ++k)
                acc += kernel[static_cast<std::size_t>(s) * (2 * kHalf + 1) + static_cast<std::size_t>(k + kHalf)] *
                       static_cast<double>(v[(n + static_cast<std::size_t>(k + 0)) * chans + ch]);
            best = std::max(best, std::fabs(acc));
        }
    return best;
}

DynamicsConfig tpConfig(std::uint32_t fs = 48000, std::uint32_t ch = 2) {
    DynamicsConfig c;
    c.sampleRateHz = fs; c.inChannels = ch;
    c.truePeakLimiter = true;
    return c;
}

struct Case { const char* name; std::vector<float> in; };

std::vector<Case> adversarialSignals(std::size_t frames, double fs) {
    std::vector<Case> v;
    // 1. fs/4 sine, 45 degrees: every sample is +-0.7071*A but the reconstructed peak is A
    v.push_back({"fs/4 sine 45deg x1.6", sineStereo(frames, fs / 4, 1.6, fs, kPi / 4)});
    // 2. near-Nyquist sine at awkward phases (the 4x-meter worst case)
    v.push_back({"0.45fs sine x1.5", sineStereo(frames, 0.45 * fs, 1.5, fs, 0.9)});
    v.push_back({"0.49fs sine x1.3", sineStereo(frames, 0.49 * fs, 1.3, fs, 2.2)});
    // 3. two-sample-period square (Nyquist/2) with Gibbs overshoot after the band limit
    v.push_back({"fs/8 square x0.95", stereo(frames, [&](std::size_t i) { return ((i / 4) % 2) ? -0.95 : 0.95; },
                                              [&](std::size_t i) { return ((i / 4) % 2) ? -0.95 : 0.95; })});
    // 4. ++-- pattern: samples +-0.9 but the reconstruction swings to ~1.27
    v.push_back({"++-- x0.9", stereo(frames, [&](std::size_t i) { return ((i / 2) % 2) ? -0.9 : 0.9; },
                                      [&](std::size_t i) { return ((i / 2) % 2) ? -0.9 : 0.9; })});
    // 5. bursty noise, deterministic
    ss::Rng rng(1234);
    std::vector<float> noise(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const double gate = ((i / 2400) % 3 == 0) ? 1.4 : 0.3;
        noise[2 * i] = static_cast<float>(gate * (rng.unit() * 2.0 - 1.0));
        noise[2 * i + 1] = static_cast<float>(gate * (rng.unit() * 2.0 - 1.0));
    }
    v.push_back({"bursty noise", noise});
    // 6. loud log sweep 100 Hz .. 0.48 fs
    {
        std::vector<float> sw(frames * 2);
        double phase = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            const double fr = 100.0 * std::pow(0.48 * fs / 100.0, static_cast<double>(i) / static_cast<double>(frames));
            phase += 2 * kPi * fr / fs;
            sw[2 * i] = sw[2 * i + 1] = static_cast<float>(1.5 * std::sin(phase));
        }
        v.push_back({"loud sweep", sw});
    }
    // 7. isolated full-scale impulses with opposite polarity neighbours (maximal inter-sample overs)
    v.push_back({"impulse pairs", stereo(frames, [&](std::size_t i) { const auto m = i % 1000; return m == 100 ? 1.0 : (m == 101 ? 1.0 : 0.0); },
                                          [&](std::size_t i) { const auto m = i % 1000; return m == 100 ? 1.0 : (m == 101 ? -1.0 : 0.0); })});
    return v;
}
}  // namespace

TEST_CASE("REQ-R02: the true-peak limiter holds the ceiling on adversarial synthetic signals (independent reference meter)") {
    const double fs = 48000.0;
    const std::size_t frames = 24000;
    for (float ceiling : {-1.0f, -3.0f, -0.5f}) {
        DynamicsParams p = neutral();
        p.ceilingDb = ceiling;
        for (const auto& c : adversarialSignals(frames, fs)) {
            Rig rig(p, tpConfig());
            const auto out = rig.run(c.in);
            REQUIRE(allFinite(out));
            // skip the first 2000 frames: the lookahead window is still filling with the (legitimately loud) start
            double worst = 0.0;
            for (std::size_t ch = 0; ch < 2; ++ch) worst = std::max(worst, referenceTruePeak(out, ch, 2, 2000, frames));
            const double db = linToDb(worst);
            INFO(std::string(c.name) << " ceiling " << ceiling << " dB read " << db << " dB");
            if (ceiling == -1.0f) MESSAGE("true-peak limiter, ceiling -1 dBTP: " << std::string(c.name) << " reads " << db << " dB");
            CHECK(db <= static_cast<double>(ceiling) + 0.1);              // spec tolerance 0.1 dB
            // and it is not simply muted: loud material keeps most of its level
            CHECK(db >= static_cast<double>(ceiling) - 12.0);
        }
    }
}

TEST_CASE("REQ-R02: the sample-peak limiter lets the same signals exceed the ceiling (the test signals are adversarial)") {
    const double fs = 48000.0;
    DynamicsParams p = neutral();
    int over = 0;
    for (const auto& c : adversarialSignals(24000, fs)) {
        DynamicsConfig cfg; cfg.truePeakLimiter = false;
        Rig rig(p, cfg);
        const auto out = rig.run(c.in);
        double worst = 0.0;
        for (std::size_t ch = 0; ch < 2; ++ch) worst = std::max(worst, referenceTruePeak(out, ch, 2, 2000, 24000));
        if (linToDb(worst) > -1.0 + 0.5) ++over;
    }
    CHECK(over >= 4);                                                    // most of the set defeats a sample-peak limiter
}

TEST_CASE("REQ-R02: below the ceiling the true-peak limiter is transparent apart from a fixed delay") {
    const double fs = 48000.0;
    for (std::uint32_t rate : {44100u, 48000u, 96000u}) {
        Rig rig(neutral(), tpConfig(rate));
        const auto in = sineStereo(20000, 997.0, 0.1, rate);              // -20 dBFS
        const auto out = rig.run(in);
        const std::size_t d = rig.info.processingDelaySamples;
        REQUIRE(d > 0);
        double maxErr = 0.0;
        for (std::size_t i = d + 10; i < 20000; ++i) maxErr = std::max(maxErr, static_cast<double>(std::fabs(out[2 * i] - in[2 * (i - d)])));
        CHECK(maxErr < 1e-6);
    }
    (void) fs;
}

TEST_CASE("REQ-R02: the true-peak limiter adds the detector latency and reports it; the default stays unchanged") {
    DynamicsConfig base; DynamicsConfig tp = base; tp.truePeakLimiter = true;
    Rig a(neutral(), base), b(neutral(), tp);
    CHECK(b.info.processingDelaySamples > a.info.processingDelaySamples);
    CHECK(b.info.processingDelaySamples - a.info.processingDelaySamples == kTruePeakExtraDelay);
    CHECK_FALSE(DynamicsConfig{}.truePeakLimiter);                                      // opt-in: existing sessions are unchanged
}

TEST_CASE("REQ-R02: bypass keeps the dry path equally delayed with the true-peak limiter on") {
    DynamicsParams p = neutral(); p.bypass = true;
    Rig rig(p, tpConfig());
    const auto in = sineStereo(12000, 440.0, 0.5, 48000);
    const auto out = rig.run(in);
    const std::size_t d = rig.info.processingDelaySamples;
    for (std::size_t i = d + 50; i < 12000; ++i) REQUIRE(out[2 * i] == doctest::Approx(in[2 * (i - d)]).epsilon(1e-5));
}

TEST_CASE("REQ-R02: mono input, hostile values and metadata with the true-peak limiter") {
    DynamicsParams p = neutral();
    Rig rig(p, tpConfig(48000, 1));
    std::vector<float> in(9000, 0.0f);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(1.4 * std::sin(2 * kPi * 0.25 * static_cast<double>(i) + kPi / 4));
    in[500] = std::nanf(""); in[501] = 1.0e30f; in[502] = -INFINITY;
    const auto out = rig.run(in);
    CHECK(allFinite(out));
    CHECK(rig.proc.telemetry().sanitizedSamples >= 2);
    const std::string j = rig.proc.metadataJson();
    CHECK(j.find("\"trueTpLimiter\":true") != std::string::npos);
    CHECK(j.find("true-peak") != std::string::npos);
    CHECK(j.find("dBTP") != std::string::npos);
    Rig plain(p, DynamicsConfig{});
    CHECK(plain.proc.metadataJson().find("\"trueTpLimiter\":false") != std::string::npos);
}

TEST_CASE("REQ-R02: gain reduction is reported and the limiter releases after a loud passage") {
    DynamicsParams p = neutral();
    Rig rig(p, tpConfig());
    std::vector<float> in = sineStereo(24000, 997.0, 1.8, 48000);       // loud
    const auto quiet = sineStereo(48000, 997.0, 0.1, 48000);
    in.insert(in.end(), quiet.begin(), quiet.end());
    const auto out = rig.run(in);
    const auto t = rig.proc.telemetry();
    CHECK(t.limiterGrMaxDb > 3.0f);
    // after ~0.5 s of quiet material the gain is back to unity (limiter release is 100 ms)
    const std::size_t d = rig.info.processingDelaySamples;
    const std::size_t i = 24000 + 30000;
    CHECK(out[2 * i] == doctest::Approx(quiet[2 * (i - 24000 - d)]).epsilon(1e-3));
}
