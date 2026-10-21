// T-DSP: FFT, band/RMS calibration, clipping, onsets, spectrum/waveform, block-size independence.
#include "doctest/doctest.h"
#include "test_util.hpp"
#include "ss/audio/analyzer.hpp"
#include "ss/audio/fft.hpp"

using namespace ss;
using namespace ss::audio;

TEST_CASE("REQ-A03: FFT matches a reference DFT and the two-for-one real packing") {
    Fft f;
    CHECK_FALSE(f.prepare(1000));
    CHECK_FALSE(f.prepare(4));
    REQUIRE(f.prepare(64));
    Rng rng(7);
    std::vector<float> a(64), b(64);
    for (auto& v : a) v = rng.range(-1.f, 1.f);
    for (auto& v : b) v = rng.range(-1.f, 1.f);
    std::vector<float> pa(33), pb(33);
    f.powerSpectra(a.data(), b.data(), pa.data(), pb.data());
    for (int k = 0; k <= 32; ++k) {
        double ra = 0, ia = 0, rb = 0, ib = 0;
        for (int n = 0; n < 64; ++n) {
            const double ang = -2 * sst::kPi * k * n / 64.0;
            ra += a[n] * std::cos(ang); ia += a[n] * std::sin(ang);
            rb += b[n] * std::cos(ang); ib += b[n] * std::sin(ang);
        }
        CHECK(pa[k] == doctest::Approx(ra * ra + ia * ia).epsilon(1e-4).scale(1.0));
        CHECK(pb[k] == doctest::Approx(rb * rb + ib * ib).epsilon(1e-4).scale(1.0));
    }
}

TEST_CASE("REQ-A03: prepare validates rate and channels") {
    Analyzer a;
    CHECK_FALSE(a.prepare(100, 2).ok());
    CHECK_FALSE(a.prepare(48000, 0).ok());
    CHECK_FALSE(a.prepare(48000, 6).ok());
    CHECK(a.prepare(48000, 2).ok());
    CHECK(a.prepare(44100, 1).ok());
    FeatureSnapshot s = a.snapshot();                    // nothing processed yet
    CHECK(s.silent);
    CHECK(s.tempo.source == ValueSource::Unknown);
    CHECK(s.key.source == ValueSource::Unknown);
}

namespace {
struct ToneCase { double f; int band; };
}

TEST_CASE("REQ-A03: calibrated tones select the expected band and RMS within 0.5 dB") {
    const ToneCase tones[] = {{100, 0}, {130, 0}, {700, 1}, {1000, 1}, {5000, 2}, {9000, 2}};
    for (std::uint32_t rate : {44100u, 48000u}) {
        for (std::uint32_t ch : {1u, 2u}) {
            for (const auto& t : tones) {
                CAPTURE(rate); CAPTURE(ch); CAPTURE(t.f);
                Analyzer a; REQUIRE(a.prepare(rate, ch).ok());
                sst::Feeder fd(a, ch, rate);
                const double amp = 0.5;
                fd.feed(sst::toChannels(sst::sine(t.f, amp, 0.5, rate), static_cast<int>(ch)), 480);
                const FeatureSnapshot s = a.snapshot();
                const double expRms = 20 * std::log10(amp / std::sqrt(2.0));
                const double expBand = 20 * std::log10(amp);       // A^2 in dB
                for (std::uint32_t c = 0; c < 2; ++c) {
                    CHECK(std::fabs(s.rmsDb[c] - expRms) < 0.5);
                    CHECK(std::fabs(s.peakDb[c] - expBand) < 0.5);
                }
                for (int b = 0; b < 3; ++b) {
                    if (b == t.band) CHECK(std::fabs(10 * std::log10(s.band[b]) - expBand) < 0.5);
                    else CHECK(s.band[b] < s.band[t.band] * 1e-3f);          // > 30 dB below
                }
                CHECK_FALSE(s.silent);
                CHECK_FALSE(s.inputNearFullScale);
            }
        }
    }
}

TEST_CASE("REQ-A03: bands are bounded by Nyquist at low sample rates") {
    Analyzer a; REQUIRE(a.prepare(8000, 1).ok());          // Nyquist 4 kHz: high band is 2-4 kHz
    sst::Feeder fd(a, 1, 8000);
    fd.feed(sst::sine(3000, 0.5, 0.5, 8000), 100);
    const FeatureSnapshot s = a.snapshot();
    CHECK(std::fabs(10 * std::log10(s.band[2]) - 20 * std::log10(0.5)) < 0.5);
    CHECK(s.band[1] < s.band[2] * 1e-3f);
}

TEST_CASE("REQ-A03: anti-phase stereo does not cancel; channels are metered separately") {
    Analyzer a; REQUIRE(a.prepare(48000, 2).ok());
    sst::Feeder fd(a, 2, 48000);
    fd.feed(sst::toChannels(sst::sine(1000, 0.5, 0.5, 48000), 2, true), 480);
    const FeatureSnapshot s = a.snapshot();
    CHECK(std::fabs(10 * std::log10(s.band[1]) - 20 * std::log10(0.5)) < 0.5);
    // different levels per channel
    Analyzer b; REQUIRE(b.prepare(48000, 2).ok());
    sst::Feeder fb(b, 2, 48000);
    auto l = sst::sine(1000, 0.5, 0.5, 48000), r = sst::sine(1000, 0.05, 0.5, 48000);
    std::vector<float> st(l.size() * 2);
    for (std::size_t i = 0; i < l.size(); ++i) { st[2 * i] = l[i]; st[2 * i + 1] = r[i]; }
    fb.feed(st, 333);
    const FeatureSnapshot t = b.snapshot();
    CHECK(t.rmsDb[0] - t.rmsDb[1] == doctest::Approx(20.0).epsilon(0.02));
}

TEST_CASE("REQ-A03: results do not depend on the block size (1, 7, 64, 480, 1023, 4096)") {
    const auto sig = sst::toChannels(sst::sine(440, 0.3, 0.6, 48000), 2);
    FeatureSnapshot ref;
    bool first = true;
    for (std::size_t block : {std::size_t(1), std::size_t(7), std::size_t(64), std::size_t(480), std::size_t(1023), std::size_t(4096)}) {
        Analyzer a; REQUIRE(a.prepare(48000, 2).ok());
        sst::Feeder fd(a, 2, 48000);
        fd.feed(sig, block);
        const FeatureSnapshot s = a.snapshot();
        if (first) { ref = s; first = false; continue; }
        CAPTURE(block);
        CHECK(s.rmsDb[0] == ref.rmsDb[0]);
        CHECK(s.peakDb[1] == ref.peakDb[1]);
        for (int b = 0; b < 3; ++b) CHECK(s.band[b] == ref.band[b]);
        CHECK(s.sourceNs == ref.sourceNs);
    }
}

TEST_CASE("REQ-A10: near full scale >= -0.5 dBFS and clip run length; distinct from quiet signals") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    std::vector<float> v(24000, 0.f);
    for (int i = 22000; i < 22010; ++i) v[i] = 1.0f;                        // run of 10 samples at 0 dBFS
    fd.feed(v, 480);
    FeatureSnapshot s = a.snapshot();
    CHECK(s.inputNearFullScale);
    CHECK(s.clipRunLength == 10);
    // 0.93 (-0.63 dBFS) is below the threshold
    Analyzer b; REQUIRE(b.prepare(48000, 1).ok());
    sst::Feeder fb(b, 1, 48000);
    std::vector<float> q(24000, 0.f);
    for (int i = 22000; i < 22010; ++i) q[i] = 0.93f;
    fb.feed(q, 480);
    s = b.snapshot();
    CHECK_FALSE(s.inputNearFullScale);
    CHECK(s.clipRunLength == 0);
    CHECK(s.peakDb[0] == doctest::Approx(20 * std::log10(0.93)).epsilon(0.01));
    // after >50 ms of quiet the warning clears
    fd.feed(std::vector<float>(4800, 0.f), 480);
    s = a.snapshot();
    CHECK_FALSE(s.inputNearFullScale);
    CHECK(s.clipRunLength == 0);
    CHECK(a.stats().nearFullScaleSamples == 10);
}

TEST_CASE("REQ-A10: a limiter cannot hide upstream clipping (input flag depends on input samples only)") {
    Analyzer a; REQUIRE(a.prepare(48000, 2).ok());
    sst::Feeder fd(a, 2, 48000);
    fd.feed(sst::toChannels(sst::sine(200, 1.0, 0.3, 48000), 2), 480);   // full scale sine
    const FeatureSnapshot s = a.snapshot();
    CHECK(s.inputNearFullScale);
    CHECK(s.peakDb[0] > -0.5f);
}

TEST_CASE("REQ-A03: silence is reported silent with floor dB values") {
    Analyzer a; REQUIRE(a.prepare(48000, 2).ok());
    sst::Feeder fd(a, 2, 48000);
    fd.feed(std::vector<float>(48000 * 2, 0.f), 512);
    const FeatureSnapshot s = a.snapshot();
    CHECK(s.silent);
    CHECK(s.rmsDb[0] == -120.f);
    CHECK(s.peakDb[1] == -120.f);
    CHECK(s.band[0] == 0.f);
    CHECK(s.tempo.source == ValueSource::Unknown);
    CHECK(s.tempo.bpm == 0.0);
    CHECK(s.key.source == ValueSource::Unknown);
    CHECK(s.key.tonic == -1);
    CHECK(s.energyArc == 0.f);
    CHECK(s.arcPhase == 0);
}

TEST_CASE("REQ-A03: kAudioSilent flag and null buffers are treated as zeros") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    fd.feed(sst::sine(1000, 0.5, 0.3, 48000), 480);
    CHECK_FALSE(a.snapshot().silent);
    std::vector<float> loud(4800, 0.9f);                                // content must be ignored
    fd.feed(loud, 480, kAudioSilent);                                    // only first block flagged: feed manually
    for (int i = 0; i < 20; ++i) {
        AudioBlockView v; v.interleaved = loud.data(); v.frames = 480; v.channels = 1; v.rateHz = 48000;
        v.flags = kAudioSilent; v.firstSampleIndex = fd.idx; fd.idx += 480;
        a.process(v);
    }
    CHECK(a.snapshot().silent);
    AudioBlockView n; n.interleaved = nullptr; n.frames = 480; n.channels = 1; n.rateHz = 48000;
    a.process(n);                                                        // must not crash
    CHECK(a.snapshot().silent);
}

// ---- onsets ---------------------------------------------------------------------------------------------
namespace {
struct Truth { std::vector<double> t; };

// Pulse corpus: seeded random intervals 0.25-1.1 s, mixed timbres and amplitudes, quiet noise bed.
std::vector<float> pulseCorpus(std::uint32_t rate, std::uint64_t seed, double sec, std::vector<double>& truth, double noiseDb) {
    Rng rng(seed);
    std::vector<float> v(static_cast<std::size_t>(sec * rate), 0.f);
    for (auto& s : v) s = static_cast<float>((rng.unit() * 2 - 1) * sst::dbToLin(noiseDb));
    double t = 0.5;
    int k = 0;
    while (t < sec - 0.3) {
        const auto h = sst::hit(k++, rng.range(0.25f, 0.9f), rate, rng);
        sst::addInPlace(v, h, static_cast<std::size_t>(t * rate));
        truth.push_back(t);
        t += rng.range(0.25f, 1.1f);
    }
    return v;
}
}

TEST_CASE("REQ-A04: synthetic pulse corpus: >= 95% matched within +-50 ms, <= 5% false detections") {
    int total = 0, matched = 0, detected = 0, falseCount = 0;
    for (std::uint32_t rate : {48000u, 44100u}) {
        for (int corpus = 0; corpus < 4; ++corpus) {
            std::vector<double> truth;
            const auto sig = pulseCorpus(rate, 100 + corpus, 10.0, truth, corpus < 2 ? -70.0 : -50.0);
            Analyzer a; REQUIRE(a.prepare(rate, 1).ok());
            sst::Feeder fd(a, 1, rate);
            fd.feed(sig, 256 + 37 * corpus);
            std::vector<OnsetEvent> ev(256);
            const std::size_t n = a.drainOnsets(ev.data(), ev.size());
            std::vector<bool> used(n, false);
            for (double t : truth) {
                ++total;
                for (std::size_t i = 0; i < n; ++i) {
                    if (!used[i] && std::fabs(nsToSec(ev[i].timeNs) - t) <= 0.05) { used[i] = true; ++matched; break; }
                }
            }
            detected += static_cast<int>(n);
            for (std::size_t i = 0; i < n; ++i) if (!used[i]) ++falseCount;
            // refractory: no two events closer than 100 ms
            for (std::size_t i = 1; i < n; ++i) CHECK(nsToMs(ev[i].timeNs - ev[i - 1].timeNs) >= 99.0);
        }
    }
    INFO("matched " << matched << "/" << total << " false " << falseCount << "/" << detected);
    CHECK(matched >= static_cast<int>(0.95 * total));
    CHECK(falseCount <= static_cast<int>(0.05 * detected));
}

TEST_CASE("REQ-A04: onsetFired latches until snapshot() and survives many blocks; steady tones do not fire") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    std::vector<float> v(48000, 0.f);
    Rng hr(3);
    sst::addInPlace(v, sst::hit(1, 0.8, 48000, hr), 20000);
    fd.feed(v, 64);                                                      // many blocks after the hit
    FeatureSnapshot s = a.snapshot();
    CHECK(s.onsetFired);
    CHECK(s.onsetStrength > 0.3f);
    s = a.snapshot();
    CHECK_FALSE(s.onsetFired);
    // steady sine: zero onsets after settling
    Analyzer b; REQUIRE(b.prepare(48000, 1).ok());
    sst::Feeder fb(b, 1, 48000);
    fb.feed(sst::sine(440, 0.5, 3.0, 48000), 480);
    CHECK(b.stats().onsets <= 1);                                       // at most the tone start
}

TEST_CASE("REQ-A04: discontinuity flag flushes the window and does not create a false onset") {
    Analyzer a; REQUIRE(a.prepare(48000, 1).ok());
    sst::Feeder fd(a, 1, 48000);
    fd.feed(sst::sine(440, 0.5, 1.0, 48000), 480);
    const auto before = a.stats().onsets;
    // gap of 30000 samples (flagged); signal continues at a different phase
    fd.idx += 30000;
    fd.feed(sst::sine(440, 0.5, 1.0, 48000, 2.0), 480, kAudioDiscontinuity);
    CHECK(a.stats().discontinuities == 1);
    // only the tone restart may produce an onset (the zero-flushed window), never more than one
    CHECK(a.stats().onsets <= before + 1);
    // timeline advanced by the gap
    CHECK(nsToSec(a.snapshot().sourceNs) == doctest::Approx((48000.0 * 2 + 30000.0) / 48000.0).epsilon(0.001));
}

TEST_CASE("REQ-A03: spectrum has 32 log bins and tracks a tone; waveform mirrors the input") {
    Analyzer a; REQUIRE(a.prepare(48000, 2).ok());
    sst::Feeder fd(a, 2, 48000);
    fd.feed(sst::toChannels(sst::sine(1000, 0.5, 0.3, 48000), 2), 480);
    float sp[32];
    a.copySpectrum(sp);
    int peak = 0;
    for (int i = 0; i < 32; ++i) { CHECK(sp[i] >= 0.f); CHECK(sp[i] <= 1.f); if (sp[i] > sp[peak]) peak = i; }
    // bin edges: 30 * (16000/30)^(i/32); 1 kHz lies in bin ~18.6
    const double expect = 32.0 * std::log(1000.0 / 30.0) / std::log(16000.0 / 30.0);
    CHECK(std::fabs(peak + 0.5 - expect) <= 1.6);
    CHECK(sp[peak] > 0.8f);
    CHECK(sp[2] < 0.3f);
    float wf[256];
    a.copyWaveform(wf);
    float mx = 0.f; for (float x : wf) { CHECK(std::fabs(x) <= 1.f); mx = std::max(mx, std::fabs(x)); }
    CHECK(mx > 0.4f);
    CHECK(mx <= 0.51f);
    // low rate: bins above Nyquist are zero
    Analyzer b; REQUIRE(b.prepare(8000, 1).ok());
    sst::Feeder fb(b, 1, 8000);
    fb.feed(sst::sine(1000, 0.5, 0.5, 8000), 100);
    b.copySpectrum(sp);
    CHECK(sp[31] == 0.f);
    CHECK(sp[30] == 0.f);
}
