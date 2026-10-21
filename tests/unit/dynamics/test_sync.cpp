// T-SYNC: clock map, drift controller, delay compensation, linear resampler (pure logic, deterministic).
#include <cmath>
#include "doctest/doctest.h"
#include "dsp_helpers.hpp"
#include "ss/dynamics/sync.hpp"

using namespace ss;
using namespace ss::dynamics;

namespace {
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kPacket = 480;   // 10 ms

// Device clock runs (1+ppm) times nominal relative to the host clock, packets carry +-jitterMs timestamp noise.
struct Sim {
    double ppm; double jitterMs; Rng rng{99}; Ns host0 = 5'000'000'000LL;
    std::uint64_t pos = 0;
    bool next(std::uint64_t& p, Ns& h) {
        pos += kPacket;
        p = pos;
        const double ideal = static_cast<double>(pos) * (1e9 / kRate) / (1.0 + ppm * 1e-6);
        const double j = jitterMs > 0 ? static_cast<double>(rng.range(-1.0f, 1.0f)) * jitterMs * 1e6 : 0.0;
        h = host0 + static_cast<Ns>(std::llround(ideal + j));
        return true;
    }
};

double estimatePpm(double ppm, double seconds, double jitterMs, ClockMap* out = nullptr, ClockEstimate* est = nullptr) {
    ClockMap map;
    Sim s{ppm, jitterMs};
    for (int i = 0; i < static_cast<int>(seconds * 100); ++i) { std::uint64_t p; Ns h; s.next(p, h); map.addPacket(p, h); }
    if (est) *est = map.estimate();
    if (out) *out = map;
    return map.estimate().valid ? map.estimate().ratePpm : 1e9;
}
}  // namespace

TEST_CASE("REQ-R03: ClockMap recovers an injected +150 ppm drift under timestamp jitter") {
    ClockEstimate e;
    const double ppm = estimatePpm(150.0, 120.0, 0.5, nullptr, &e);
    CHECK(e.valid);
    CHECK(std::fabs(ppm - 150.0) < 10.0);
    CHECK(e.residualMadNs > 1e4);   // jitter is visible in the residual (it is ~0.25 ms)
    CHECK(std::fabs(estimatePpm(-150.0, 120.0, 0.5) + 150.0) < 10.0);
    CHECK(std::fabs(estimatePpm(0.0, 120.0, 0.5)) < 10.0);
    CHECK(std::fabs(estimatePpm(150.0, 120.0, 0.0) - 150.0) < 0.5);   // exact without jitter
}

TEST_CASE("REQ-R03: +300 ppm is measured and flagged as an error by the DriftController") {
    ClockEstimate e;
    const double ppm = estimatePpm(300.0, 120.0, 0.5, nullptr, &e);
    CHECK(std::fabs(ppm - 300.0) < 10.0);
    DriftController dc;
    const DriftDecision& d = dc.update(e);
    CHECK(d.error);
    CHECK(d.state == DriftState::Error);
    CHECK(dc.errorEntries() == 1);
    CHECK(d.appliedPpm == 0.0);            // never hides a large mismatch with resampling
    CHECK(d.inPerOut == 1.0);
    // negative direction too
    ClockEstimate en;
    estimatePpm(-300.0, 120.0, 0.5, nullptr, &en);
    DriftController dn;
    CHECK(dn.update(en).error);
}

TEST_CASE("REQ-R03: ClockMap validity, outlier robustness, monotonicity and mapping") {
    ClockMap m;
    Sim s{150.0, 0.2};
    for (int i = 0; i < 500; ++i) { std::uint64_t p; Ns h; s.next(p, h); m.addPacket(p, h); }   // 5 s only
    CHECK_FALSE(m.estimate().valid);
    CHECK(m.hostNsForSample(480 * 100) != 0);   // still predicts (nominal rate) before validity

    ClockMap k;
    Sim s2{-80.0, 0.2};
    std::uint64_t lastP = 0; Ns lastH = 0;
    for (int i = 0; i < 6000; ++i) {
        std::uint64_t p; Ns h; s2.next(p, h);
        if (i % 50 == 25) h += 80'000'000;      // 80 ms scheduling stall on a packet
        k.addPacket(p, h);
        if (i % 200 == 199) (void)k.estimate();   // outliers are judged against the latest fit
        lastP = p; lastH = h;
    }
    CHECK(k.estimate().valid);
    CHECK(std::fabs(k.estimate().ratePpm + 80.0) < 10.0);
    CHECK(k.outliers() > 0);
    // mapping error against the true line is far below a video frame (16.7 ms) after a minute
    Sim truth{-80.0, 0.0};
    std::uint64_t tp = 0; Ns th = 0;
    for (int i = 0; i < 6000; ++i) truth.next(tp, th);
    CHECK(std::llabs(k.hostNsForSample(tp) - th) < 500'000);
    CHECK(std::llabs(static_cast<Ns>(k.sampleForHostNs(th)) - static_cast<Ns>(tp)) < 30);
    (void)lastP; (void)lastH;

    // non-monotonic pairs are rejected, not absorbed
    const auto before = k.size();
    const auto rejBefore = k.rejectedNonMonotonic();
    CHECK_FALSE(k.addPacket(10, 100));
    CHECK(k.rejectedNonMonotonic() == rejBefore + 1);
    CHECK(k.size() == before);
    k.reset();
    CHECK(k.size() == 0);
    CHECK_FALSE(k.estimate().valid);
}

TEST_CASE("REQ-R03: DriftController corrects gently within +-200 ppm and recovers from error with hysteresis") {
    DriftController dc;
    ClockEstimate e; e.valid = true; e.ratePpm = 150.0;
    double prev = 0.0;
    for (int i = 0; i < 30; ++i) {
        const auto& d = dc.update(e);
        CHECK(d.appliedPpm - prev <= 10.0 + 1e-9);    // slew limited
        CHECK(d.appliedPpm <= 150.0 + 1e-9);
        CHECK_FALSE(d.error);
        prev = d.appliedPpm;
    }
    CHECK(prev == doctest::Approx(150.0));
    CHECK(dc.decision().state == DriftState::Correcting);
    CHECK(dc.decision().inPerOut == doctest::Approx(1.00015).epsilon(1e-9));
    // deadband: tiny error leaves the ratio alone
    DriftController quiet;
    ClockEstimate tiny; tiny.valid = true; tiny.ratePpm = 1.0;
    quiet.update(tiny);
    CHECK(quiet.decision().state == DriftState::Locked);
    CHECK(quiet.decision().inPerOut == 1.0);
    // 201 ppm is an error; 185 ppm does not clear it (hysteresis); 170 ppm does
    e.ratePpm = 201.0;
    CHECK(dc.update(e).error);
    CHECK(dc.decision().appliedPpm == doctest::Approx(150.0));   // keeps the correction already in force
    e.ratePpm = 185.0;
    CHECK(dc.update(e).error);
    e.ratePpm = 170.0;
    CHECK_FALSE(dc.update(e).error);
    // invalid estimate holds state
    ClockEstimate none;
    CHECK(dc.update(none).appliedPpm > 0.0);
    CHECK(dc.correctionChanges() > 10);
}

TEST_CASE("REQ-R03: LinearResampler is exact at ratio 1, chunk-independent, and adds/drops one frame per 1/ppm") {
    LinearResampler r(2);
    std::vector<float> in(2000 * 2);
    for (std::size_t i = 0; i < 2000; ++i) { in[2 * i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i))); in[2 * i + 1] = -in[2 * i]; }
    std::vector<float> out(2100 * 2);
    auto io = r.process(in.data(), 2000, out.data(), 2100);
    CHECK(io.consumed == 2000);
    CHECK(io.produced == 2000);
    CHECK(out[0] == 0.0f);                              // one frame of zero history
    for (std::size_t i = 1; i < 2000; ++i) CHECK(out[2 * i] == in[2 * (i - 1)]);

    // chunk independence with a non-trivial ratio
    const double ratio = 1.0 + 200e-6;
    LinearResampler a(2), b(2);
    a.setRatio(ratio); b.setRatio(ratio);
    std::vector<float> long1(100000 * 2);
    for (std::size_t i = 0; i < 100000; ++i) long1[2 * i] = long1[2 * i + 1] = static_cast<float>(std::sin(0.02 * static_cast<double>(i)));
    std::vector<float> oa(100100 * 2), ob(100100 * 2);
    auto ia = a.process(long1.data(), 100000, oa.data(), 100100);
    std::size_t consumed = 0, produced = 0;
    ss::Rng rng(5);
    while (consumed < 100000) {
        const std::size_t chunk = std::min<std::size_t>(1 + rng.below(700), 100000 - consumed);
        auto x = b.process(long1.data() + consumed * 2, chunk, ob.data() + produced * 2, 100100 - produced);
        CHECK(x.consumed == chunk);
        consumed += x.consumed; produced += x.produced;
    }
    CHECK(produced == ia.produced);
    for (std::size_t i = 0; i < produced * 2; ++i) CHECK(oa[i] == ob[i]);
    // 100000 input frames at 200 ppm: 20 fewer output frames
    CHECK(std::llabs(static_cast<long long>(100000) - static_cast<long long>(produced) - 20) <= 1);

    // output cap smaller than needed: partial consume, remainder can be re-submitted
    LinearResampler c(1);
    std::vector<float> mono(1000, 0.5f), small(10);
    auto p1 = c.process(mono.data(), 1000, small.data(), 10);
    CHECK(p1.produced == 10);
    CHECK(p1.consumed <= 11);
    // ratio clamp / NaN safety
    c.setRatio(std::nan(""));
    CHECK(c.ratio() == 1.0);
    c.setRatio(100.0);
    CHECK(c.ratio() == 2.0);
}

TEST_CASE("REQ-R03: end to end, a +150 ppm device is held within a few ms of host time over two minutes") {
    ClockMap map;
    DriftController dc;
    LinearResampler rs(1);
    Sim s{150.0, 0.3};
    std::vector<float> in(kPacket, 0.1f), out(kPacket + 8);
    std::uint64_t outFrames = 0, inFrames = 0;
    Ns lastHost = 0;
    for (int i = 0; i < 12000; ++i) {
        std::uint64_t p; Ns h; s.next(p, h);
        map.addPacket(p, h);
        if (i % 100 == 99) { dc.update(map.estimate()); rs.setRatio(dc.decision().inPerOut); }
        auto io = rs.process(in.data(), kPacket, out.data(), out.size());
        inFrames += io.consumed; outFrames += io.produced;
        lastHost = h;
    }
    CHECK_FALSE(dc.decision().error);
    const double elapsed = static_cast<double>(lastHost - s.host0) / 1e9;
    const double idealFrames = elapsed * kRate;
    const double uncorrectedErrMs = (static_cast<double>(inFrames) - idealFrames) / kRate * 1e3;
    const double correctedErrMs = (static_cast<double>(outFrames) - idealFrames) / kRate * 1e3;
    MESSAGE("uncorrected " << uncorrectedErrMs << " ms, corrected " << correctedErrMs << " ms");
    CHECK(uncorrectedErrMs > 15.0);
    CHECK(std::fabs(correctedErrMs) < 6.0);
}

TEST_CASE("REQ-R03: delay compensation record totals lookahead, filter and codec priming and offsets PTS") {
    DynamicsConfig cfg; cfg.sampleRateHz = 48000;
    DynamicsProcessor proc;
    auto info = proc.prepare(cfg).value();
    DelayCompensation dc = makeDelayCompensation(info, 24, 1024);
    CHECK(dc.lookaheadSamples == 240);
    CHECK(dc.totalSamples() == 240u + 24u + 1024u);
    CHECK(dc.totalNs() == samplesToNs(1288, 48000));
    CHECK(dc.videoPtsOffsetNs() == dc.totalNs());
    // the first real content sample (output index == total delay) lands at PTS 0
    CHECK(dc.audioPtsNs(1288) == 0);
    CHECK(dc.audioPtsNs(48000 + 1288) == kNsPerSec);
    CHECK(dc.toJson().find("\"totalSamples\":1288") != std::string::npos);
}
