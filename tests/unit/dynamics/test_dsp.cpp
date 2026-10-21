// T-DSP: compression curve, limiter ceiling, linking, silence, robustness, bypass, meters, true-peak meter.
#include <cstring>
#include "doctest/doctest.h"
#include "dsp_helpers.hpp"
#include "ss/dynamics/truepeak.hpp"

using namespace dyntest;

namespace {
constexpr double kFs = 48000.0;

// Independent reference of the static curve, written as output level (Giannoulis/Massberg/Reiss form).
double refOutDb(double L, double T, double R, double W) {
    const double over = L - T;
    if (W <= 0.0) return over <= 0.0 ? L : T + over / R;
    if (2.0 * over < -W) return L;
    if (2.0 * std::fabs(over) <= W) { const double x = over + W / 2.0; return L + (1.0 / R - 1.0) * x * x / (2.0 * W); }
    return T + over / R;
}
}  // namespace

TEST_CASE("REQ-R02: defaults and ranges match the section 12 table; out-of-range is clamped and reported") {
    DynamicsParams d;
    CHECK(d.trimDb == 0.0f); CHECK(d.thresholdDb == -12.0f); CHECK(d.ratio == 3.0f);
    CHECK(d.attackMs == 10.0f); CHECK(d.releaseMs == 150.0f); CHECK(d.kneeDb == 6.0f);
    CHECK(d.makeupDb == 0.0f); CHECK(d.ceilingDb == -1.0f); CHECK(d.limiterReleaseMs == 100.0f);
    CHECK_FALSE(d.bypass);
    DynamicsConfig c;
    CHECK(c.lookaheadMs == 5.0f);

    ParamReport rep;
    (void)clampParams(d, &rep);
    CHECK_FALSE(rep.anyClamped());

    DynamicsParams bad;
    bad.trimDb = -100; bad.thresholdDb = 5; bad.ratio = 50; bad.attackMs = 0.01f; bad.releaseMs = 9999;
    bad.kneeDb = -1; bad.makeupDb = 40; bad.ceilingDb = 0.0f; bad.limiterReleaseMs = 1;
    DynamicsParams cl = clampParams(bad, &rep);
    CHECK(cl.trimDb == -24.0f); CHECK(cl.thresholdDb == 0.0f); CHECK(cl.ratio == 10.0f); CHECK(cl.attackMs == 0.5f);
    CHECK(cl.releaseMs == 2000.0f); CHECK(cl.kneeDb == 0.0f); CHECK(cl.makeupDb == 12.0f); CHECK(cl.ceilingDb == -0.5f);
    CHECK(cl.limiterReleaseMs == 20.0f);
    CHECK(rep.clampedMask == (kPTrim | kPThreshold | kPRatio | kPAttack | kPRelease | kPKnee | kPMakeup | kPCeiling | kPLimiterRelease));
    CHECK(rep.describe().find("ceiling") != std::string::npos);

    DynamicsParams hi; hi.trimDb = 99; hi.ceilingDb = -99; hi.ratio = 0.5f;
    cl = clampParams(hi, &rep);
    CHECK(cl.trimDb == 12.0f); CHECK(cl.ceilingDb == -12.0f); CHECK(cl.ratio == 1.0f);

    DynamicsParams nan; nan.ratio = std::nanf(""); nan.thresholdDb = INFINITY;
    cl = clampParams(nan, &rep);
    CHECK(cl.ratio == 3.0f); CHECK(cl.thresholdDb == -12.0f);
    CHECK(rep.clampedMask == (kPRatio | kPThreshold));

    DynamicsProcessor p;
    ParamReport r2 = p.setParams(bad);
    CHECK(r2.anyClamped());
    CHECK(p.params().ceilingDb == -0.5f);
}

TEST_CASE("REQ-R02: static compression curve error <= 0.1 dB across ratios, knees, thresholds") {
    double worst = 0.0;
    const double ratios[] = {1.5, 3.0, 6.0, 10.0};
    const double knees[] = {0.0, 6.0, 12.0};
    const double thresholds[] = {-12.0, -24.0};
    for (double thr : thresholds)
        for (double ratio : ratios)
            for (double knee : knees)
                for (double L = -40.0; L <= -1.0; L += 3.0) {
                    DynamicsParams p;
                    p.thresholdDb = static_cast<float>(thr); p.ratio = static_cast<float>(ratio); p.kneeDb = static_cast<float>(knee);
                    p.attackMs = 2.0f; p.ceilingDb = -0.5f;
                    Rig rig(p);
                    const std::size_t n = 19200;
                    auto in = sineStereo(n, 1000.0, dbToLin(L), kFs);
                    auto out = rig.run(in);
                    const double measured = linToDb(peakAbs(out, (n - 2400) * 2));
                    const double expected = refOutDb(L, thr, ratio, knee);
                    const double err = std::fabs(measured - expected);
                    worst = std::max(worst, err);
                    CHECK_MESSAGE(err <= 0.1, "thr=" << thr << " ratio=" << ratio << " knee=" << knee << " L=" << L << " got " << measured << " want " << expected);
                }
    MESSAGE("worst static curve error (dB): " << worst);
}

TEST_CASE("REQ-R02: trim and makeup shift the curve as specified") {
    DynamicsParams p;
    p.trimDb = -6.0f; p.makeupDb = 3.0f; p.ratio = 4.0f; p.kneeDb = 0.0f; p.thresholdDb = -20.0f; p.ceilingDb = -0.5f; p.attackMs = 2.0f;
    Rig rig(p);
    const double L = -4.0;   // raw input level; after trim -10, below... above threshold -20 -> compressed
    auto out = rig.run(sineStereo(24000, 1000.0, dbToLin(L), kFs));
    const double expected = refOutDb(L - 6.0, -20.0, 4.0, 0.0) + 3.0;
    CHECK(std::fabs(linToDb(peakAbs(out, 40000)) - expected) <= 0.1);
}

TEST_CASE("REQ-R02: limiter ceiling holds within 0.1 dB on sine, impulse, clipped square, noise, near-Nyquist, asymmetry") {
    struct Stim { const char* name; std::vector<float> in; bool steadySine; };
    const std::size_t n = 48000;
    ss::Rng rng(7);
    std::vector<Stim> stims;
    stims.push_back({"sine", sineStereo(n, 1000.0, 2.0, kFs), true});
    stims.push_back({"impulse", stereo(n, [](std::size_t i) { return i % 1000 == 100 ? 4.0 : 0.0; },
                                       [](std::size_t i) { return i % 1000 == 600 ? -0.9 : 0.0; }), false});
    stims.push_back({"clipped square", stereo(n, [](std::size_t i) { return (i / 240) % 2 ? -1.5 : 1.5; },
                                              [](std::size_t i) { return (i / 240) % 2 ? -1.5 : 1.5; }), false});
    {
        std::vector<float> noise(n * 2);
        for (auto& v : noise) v = rng.range(-3.0f, 3.0f);
        stims.push_back({"full-scale noise", noise, false});
    }
    stims.push_back({"near-Nyquist transients", stereo(n, [](std::size_t i) { return (i % 500) < 8 ? ((i & 1) ? -3.0 : 3.0) : 0.0; },
                                                       [](std::size_t i) { return (i % 700) < 5 ? ((i & 1) ? 2.5 : -2.5) : 0.0; }), false});
    stims.push_back({"stereo asymmetry", stereo(n, [](std::size_t i) { return 3.0 * std::sin(2 * kPi * 440.0 * static_cast<double>(i) / kFs); },
                                                [](std::size_t i) { return 0.05 * std::sin(2 * kPi * 440.0 * static_cast<double>(i) / kFs); }), false});

    const float ceilings[] = {-0.5f, -1.0f, -6.0f, -12.0f};
    for (const auto& s : stims)
        for (float ceilDb : ceilings)
            for (int useNeutral = 0; useNeutral < 2; ++useNeutral) {
                DynamicsParams p = useNeutral ? neutral() : DynamicsParams{};
                p.ceilingDb = ceilDb;
                if (!useNeutral) p.makeupDb = 6.0f;   // push the limiter harder than the compressor alone
                Rig rig(p);
                auto out = rig.run(s.in);
                const double ceilLin = dbToLin(ceilDb);
                const double pk = peakAbs(out);
                CHECK_MESSAGE(linToDb(pk) <= ceilDb + 0.1, s.name << " ceiling " << ceilDb << " neutral=" << useNeutral << " peak " << linToDb(pk));
                CHECK(allFinite(out));
                CHECK(rig.proc.telemetry().limiterActiveSamples > 0);
                if (s.steadySine && useNeutral) {   // steady state sits on the ceiling, not below it
                    const double steady = linToDb(peakAbs(out, (n - 4800) * 2));
                    CHECK(std::fabs(steady - ceilDb) <= 0.1);
                    CHECK(peakAbs(out, (n - 4800) * 2) <= static_cast<float>(ceilLin) * 1.0001f);
                }
            }
}

TEST_CASE("REQ-R02: startup is the lookahead delay: first D output frames are silent, ceiling holds from sample 0") {
    Rig rig(neutral());
    auto out = rig.run(stereo(2000, [](std::size_t) { return 5.0; }, [](std::size_t) { return -5.0; }));
    for (std::uint32_t i = 0; i < rig.info.processingDelaySamples * 2; ++i) CHECK(out[i] == 0.0f);
    CHECK(peakAbs(out) <= static_cast<float>(dbToLin(-1.0)) * 1.0001f);
}

TEST_CASE("REQ-R02: compressor and limiter gain is channel-linked (balance preserved)") {
    DynamicsParams p;
    p.makeupDb = 6.0f;
    Rig rig(p);
    auto in = stereo(48000, [](std::size_t i) { return 3.0 * std::sin(2 * kPi * 300.0 * static_cast<double>(i) / kFs); },
                     [](std::size_t i) { return 0.3 * std::sin(2 * kPi * 300.0 * static_cast<double>(i) / kFs); });
    auto out = rig.run(in);
    int checked = 0;
    for (std::size_t i = 0; i < 48000; ++i) {
        const float l = out[2 * i], r = out[2 * i + 1];
        if (std::fabs(l) > 0.01f) { CHECK(r / l == doctest::Approx(0.1).epsilon(1e-4)); ++checked; }
    }
    CHECK(checked > 10000);
    auto t = rig.proc.telemetry();
    CHECK(t.compressorGrMaxDb > 3.0f);
    CHECK(t.limiterGrMaxDb > 0.5f);
}

TEST_CASE("REQ-R02: silence in, silence out, no gain reduction") {
    Rig rig;
    auto out = rig.run(std::vector<float>(48000 * 2, 0.0f));
    CHECK(peakAbs(out) == 0.0f);
    auto t = rig.proc.telemetry();
    CHECK(t.compressorGrMaxDb == 0.0f);
    CHECK(t.limiterGrMaxDb == 0.0f);
    CHECK_FALSE(t.limiterActive);
    CHECK(t.outputPeakDb == kSilenceDb);
    CHECK(t.inputPeakDb == kSilenceDb);
    // loud burst then silence: gain recovers and the tail decays to exact zero
    auto burst = sineStereo(9600, 1000.0, 1.5, kFs);
    burst.resize(burst.size() + 96000 * 2, 0.0f);
    auto o2 = rig.run(burst);
    CHECK(allFinite(o2));
    CHECK(peakAbs(o2, o2.size() - 2000) == 0.0f);
}

TEST_CASE("REQ-R02: fuzzed floats and parameters never produce NaN or Inf") {
    ss::Rng rng(12345);
    DynamicsProcessor proc;
    DynamicsConfig cfg;
    proc.prepare(cfg);
    std::vector<float> in(512 * 2), out(512 * 2);
    for (int blk = 0; blk < 400; ++blk) {
        for (auto& v : in) {
            std::uint32_t bits = static_cast<std::uint32_t>(rng.next());
            const std::uint32_t pick = rng.below(8);
            if (pick < 3) std::memcpy(&v, &bits, 4);                 // arbitrary bit patterns: NaN, Inf, denormals, huge
            else if (pick == 3) v = (rng.below(2) ? 1.0f : -1.0f) * 1e30f;
            else if (pick == 4) v = 1e-39f;                           // denormal
            else v = rng.range(-1.5f, 1.5f);
        }
        DynamicsParams p;
        auto rnd = [&](float lo, float hi) { return rng.below(10) == 0 ? (rng.below(2) ? std::nanf("") : INFINITY) : rng.range(lo, hi); };
        p.trimDb = rnd(-40, 30); p.thresholdDb = rnd(-60, 10); p.ratio = rnd(0, 20); p.attackMs = rnd(-5, 200);
        p.releaseMs = rnd(-5, 4000); p.kneeDb = rnd(-5, 20); p.makeupDb = rnd(-5, 30); p.ceilingDb = rnd(-30, 5);
        p.limiterReleaseMs = rnd(-5, 900); p.bypass = rng.below(5) == 0;
        proc.setParams(p);
        REQUIRE(proc.process(in.data(), out.data(), 512));
        for (float v : out) { if (!std::isfinite(v)) FAIL("non-finite output at block " << blk); }
    }
    auto t = proc.telemetry();
    CHECK(t.sanitizedSamples > 0);
    CHECK(std::isfinite(t.outputPeakDb));
    CHECK(std::isfinite(t.compressorGrMaxDb));
    CHECK(std::isfinite(t.limiterGrMaxDb));
}

TEST_CASE("REQ-R02: bypass is an equally delayed dry path with click-free ramps") {
    DynamicsParams p;   // defaults: compressor active on a -6 dBFS tone
    const std::size_t n = 48000 * 2;
    auto in = sineStereo(n, 440.0, 0.5, kFs);
    Rig rig(p);
    std::vector<float> out(in.size());
    const std::uint32_t D = rig.info.processingDelaySamples;
    // toggle bypass on at frame 20000 and off at 60000, block size 100
    for (std::size_t f = 0; f < n; f += 100) {
        DynamicsParams q = p;
        q.bypass = f >= 20000 && f < 60000;
        rig.proc.setParams(q);
        rig.proc.process(in.data() + f * 2, out.data() + f * 2, 100);
    }
    // max sample-to-sample step stays near the natural slope of the tone (0.5*2*pi*440/48000 = 0.0288)
    float maxStep = 0;
    for (std::size_t i = 1; i < n; ++i) maxStep = std::max(maxStep, std::fabs(out[2 * i] - out[2 * (i - 1)]));
    CHECK_MESSAGE(maxStep < 0.04f, "max step " << maxStep);
    // after the ramp, bypassed output equals the input delayed by exactly D frames (bit exact)
    for (std::size_t i = 25000; i < 55000; ++i) CHECK(out[2 * i] == in[2 * (i - D)]);
    // compressed region differs from the dry path (processing really was active)
    bool differs = false;
    for (std::size_t i = 70000; i < 90000; ++i) differs |= out[2 * i] != in[2 * (i - D)];
    CHECK(differs);
    CHECK(rig.proc.telemetry().bypassed == false);
}

TEST_CASE("REQ-R02: parameter changes are smoothed (no zipper steps)") {
    DynamicsParams p = neutral();
    p.thresholdDb = 0.0f; p.ceilingDb = -0.5f;
    Rig rig(p);
    rig.block = 64;
    const std::size_t n = 24000;
    auto in = sineStereo(n, 100.0, 0.2, kFs);
    std::vector<float> out(in.size());
    for (std::size_t f = 0; f < n; f += 64) {
        DynamicsParams q = p;
        q.trimDb = f >= 12000 ? 12.0f : 0.0f;
        rig.proc.setParams(q);
        rig.proc.process(in.data() + f * 2, out.data() + f * 2, static_cast<std::uint32_t>(std::min<std::size_t>(64, n - f)));
    }
    float maxStep = 0;
    for (std::size_t i = 1; i < n; ++i) maxStep = std::max(maxStep, std::fabs(out[2 * i] - out[2 * (i - 1)]));
    CHECK_MESSAGE(maxStep < 0.03f, "max step " << maxStep);   // an unsmoothed +12 dB step would jump by up to 0.6
    CHECK(linToDb(peakAbs(out, (n - 2400) * 2)) == doctest::Approx(linToDb(0.2) + 12.0).epsilon(0.01));
}

TEST_CASE("REQ-A10: raw input clip flags are distinct from limiter-controlled output meters") {
    DynamicsParams p = neutral();
    p.ceilingDb = -6.0f;
    Rig rig(p);
    auto loud = stereo(9600, [](std::size_t i) { return (i / 60) % 2 ? -1.0 : 1.0; }, [](std::size_t i) { return 0.2 * std::sin(0.01 * static_cast<double>(i)); });
    auto out = rig.run(loud);
    auto t = rig.proc.telemetry();
    CHECK(t.inputNearFullScale);
    CHECK(t.inputPossibleClipping);
    CHECK(t.inputClipRunMax >= 60);
    CHECK(t.inputPeakDb == doctest::Approx(0.0).epsilon(0.01));
    CHECK(t.outputPeakDb <= -6.0f + 0.1f);          // limiter held the output below the ceiling...
    CHECK(t.limiterActive);                          // ...while the raw input warning stays raised
    CHECK(t.limiterActiveSamples > 0);
    CHECK(t.outputOverFullScaleSamples == 0);
    CHECK(peakAbs(out) <= static_cast<float>(dbToLin(-6.0)) * 1.0001f);
    // metadata keeps the two separate
    const std::string j = rig.proc.metadataJson();
    CHECK(j.find("\"rawInput\"") != std::string::npos);
    CHECK(j.find("\"limiterControlledOutput\"") != std::string::npos);
    CHECK(j.find("\"possibleClipping\":true") != std::string::npos);
    CHECK(j.find("\"trueTpLimiter\":false") != std::string::npos);
    // holds clear on request; quiet input raises no warning
    rig.run(std::vector<float>(48000 * 2, 0.0f));   // let the limiter release fully
    rig.proc.resetHolds();
    rig.run(sineStereo(4800, 440.0, 0.1, kFs));
    t = rig.proc.telemetry();
    CHECK_FALSE(t.inputNearFullScale);
    CHECK_FALSE(t.inputPossibleClipping);
    CHECK(t.inputPeakDb < -19.0f);
    CHECK(t.limiterGrMaxDb < 0.01f);   // exponential release has not quite reached unity
}

TEST_CASE("REQ-R02: delay report equals the measured impulse delay (wet and bypassed, 44.1 and 48 kHz)") {
    for (std::uint32_t rate : {44100u, 48000u})
        for (int bypass = 0; bypass < 2; ++bypass) {
            DynamicsParams p;
            p.bypass = bypass != 0;
            DynamicsConfig cfg; cfg.sampleRateHz = rate;
            Rig rig(p, cfg);
            const std::size_t at = 1000;
            std::vector<float> in(4000 * 2, 0.0f);
            in[at * 2] = 0.05f;   // below threshold: unity gain
            auto out = rig.run(in);
            std::size_t first = 0;
            while (first < 4000 && out[first * 2] == 0.0f) ++first;
            CHECK(first - at == rig.info.processingDelaySamples);
            CHECK(rig.proc.telemetry().processingDelaySamples == rig.info.processingDelaySamples);
            CHECK(rig.proc.activeInfo().processingDelaySamples == rig.info.processingDelaySamples);
            CHECK(out[first * 2] == doctest::Approx(0.05f));
            CHECK(rig.info.processingDelaySamples == static_cast<std::uint32_t>(std::lround(0.005 * rate)));
        }
}

TEST_CASE("REQ-R02: mono input with explicit stereo route; prepare validation; not-ready behaviour") {
    DynamicsConfig cfg; cfg.inChannels = 1; cfg.monoToStereo = true;
    Rig rig(DynamicsParams{}, cfg);
    CHECK(rig.info.outChannels == 2);
    std::vector<float> in(4800);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(0.7 * std::sin(0.05 * static_cast<double>(i)));
    auto out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) CHECK(out[2 * i] == out[2 * i + 1]);
    CHECK(peakAbs(out) > 0.1f);

    DynamicsConfig mono; mono.inChannels = 1;
    Rig r2(DynamicsParams{}, mono);
    CHECK(r2.info.outChannels == 1);
    CHECK(r2.run(in).size() == in.size());

    DynamicsProcessor p;
    float a[4] = {0, 0, 0, 0}, b[4] = {9, 9, 9, 9};
    CHECK_FALSE(p.process(a, b, 2));
    CHECK(b[0] == 9.0f);
    CHECK_FALSE(p.telemetry().ready);
    DynamicsConfig bad; bad.sampleRateHz = 100;
    CHECK_FALSE(p.prepare(bad).ok());
    bad = DynamicsConfig{}; bad.inChannels = 3;
    CHECK(p.prepare(bad).error().category == ss::ErrorCategory::UnsupportedCapability);
    bad = DynamicsConfig{}; bad.lookaheadMs = 500.0f;
    CHECK(p.prepare(bad).error().category == ss::ErrorCategory::InvalidArgument);
    bad = DynamicsConfig{}; bad.lookaheadMs = std::nanf("");
    CHECK_FALSE(p.prepare(bad).ok());
}

TEST_CASE("REQ-P05: prepare() while running stages a new engine that is adopted at a block boundary") {
    DynamicsProcessor p;
    DynamicsConfig c1; c1.lookaheadMs = 5.0f;
    REQUIRE(p.prepare(c1).ok());
    std::vector<float> in(480 * 2, 0.1f), out(480 * 2);
    p.process(in.data(), out.data(), 480);
    CHECK(p.activeInfo().processingDelaySamples == 240);
    DynamicsConfig c2; c2.lookaheadMs = 10.0f;
    REQUIRE(p.prepare(c2).value().processingDelaySamples == 480);
    CHECK(p.activeInfo().processingDelaySamples == 240);   // not yet adopted
    p.process(in.data(), out.data(), 480);
    CHECK(p.activeInfo().processingDelaySamples == 480);
    DynamicsConfig c3; c3.lookaheadMs = 2.0f;
    REQUIRE(p.prepare(c3).ok());
    REQUIRE(p.prepare(c3).ok());                           // a staged-but-unadopted engine is replaced, not leaked
    p.process(in.data(), out.data(), 480);
    CHECK(p.activeInfo().processingDelaySamples == 96);
    CHECK(allFinite(out));
}

TEST_CASE("REQ-R02: true-peak 4x meter estimates inter-sample peaks; sample-peak limiter is NOT true-peak safe") {
    // Steady-state accuracy of the 4x meter against the analytic peak (a unit sine reads 0 dB). The first 200 samples are
    // skipped: an abruptly started sine is a step and legitimately overshoots (Gibbs) in any band-limited interpolator.
    // Inherent 4x limit: the nearest interpolated point can be 1/8 sample from the true peak, i.e. under-read by
    // 20*log10(cos(pi*f/4)) (0.17 dB at fs/4, 0.55 dB at 0.45 fs). Over-read must stay within 0.1 dB.
    double worstLow = 0, worstHigh = 0;
    for (double f : {0.02, 0.1, 0.2, 0.25, 0.33, 0.4, 0.45}) {
        const double inherent = 20.0 * std::log10(std::cos(kPi * f / 4.0));
        for (double ph : {0.0, 0.7, 1.9, 3.3, kPi / 4}) {
            ss::dynamics::TruePeakMeter m;
            double steady = 0;
            for (int i = 0; i < 2000; ++i) {
                const float v = m.push(static_cast<float>(std::sin(2 * kPi * f * i + ph)));
                if (i >= 200) steady = std::max(steady, static_cast<double>(v));
            }
            const double e = linToDb(steady);
            worstLow = std::min(worstLow, e); worstHigh = std::max(worstHigh, e);
            CHECK_MESSAGE(e <= 0.1, "f=" << f << " ph=" << ph << " read " << e);
            CHECK_MESSAGE(e >= inherent - 0.1, "f=" << f << " ph=" << ph << " read " << e << " inherent " << inherent);
        }
    }
    MESSAGE("true-peak meter steady error range up to 0.45 fs: " << worstLow << " .. " << worstHigh << " dB");

    // fs/4 sine at 45 degrees: every sample is +-0.7071 but the reconstructed peak is 1.0
    const std::size_t n = 4000;
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<float>(std::sin(2 * kPi * 0.25 * static_cast<double>(i) + kPi / 4));
    CHECK(peakAbs(s) == doctest::Approx(0.7071f).epsilon(0.001));
    {
        ss::dynamics::TruePeakMeter m;
        double steady = 0;
        for (std::size_t i = 0; i < n; ++i) { const float v = m.push(s[i]); if (i >= 200) steady = std::max(steady, static_cast<double>(v)); }
        CHECK(std::fabs(linToDb(steady)) <= 0.1);
    }
    CHECK(ss::dynamics::truePeakOf(s.data(), n) > 0.99f);   // offline helper also sees it (and the start transient)

    // Honest limiter status: sample-peak limiting at -1 dBFS lets the reconstructed peak exceed the ceiling.
    DynamicsParams p = neutral();
    Rig rig(p);
    std::vector<float> in(48000 * 2);
    for (std::size_t i = 0; i < 48000; ++i) in[2 * i] = in[2 * i + 1] = static_cast<float>(2.0 * std::sin(2 * kPi * 0.25 * static_cast<double>(i) + kPi / 4));
    auto out = rig.run(in);
    auto t = rig.proc.telemetry();
    CHECK(t.outputPeakDb <= -1.0f + 0.1f);          // sample ceiling holds
    CHECK(t.outputTruePeakDb > -1.0f + 0.5f);       // true peak is above it: no true-peak guarantee is claimed
}
