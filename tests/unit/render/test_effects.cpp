#include <algorithm>
#include <cmath>
#include <vector>
#include <chrono>
#include "test_util.hpp"

using namespace ss;
using namespace ss::render;
using ss::model::EffectKind;
using ss::model::SceneKind;

namespace {
PixelBuffer baseFrame(std::uint32_t w = 160, std::uint32_t h = 120) {
    rt::Fixture fx;
    return rt::renderOnce(SceneKind::PlasmaLite, w, h, fx.in());
}
PixelBuffer dotFrame(std::uint32_t w, std::uint32_t h) {
    PixelBuffer b(w, h);
    for (std::size_t i = 3; i < b.rgba.size(); i += 4) b.rgba[i] = 255;
    for (int dy = -8; dy < 8; ++dy)
        for (int dx = -8; dx < 8; ++dx) {
            std::uint8_t* p = b.row(h / 2 + dy) + (w / 2 + dx) * 4;
            p[0] = p[1] = p[2] = 255;
        }
    return b;
}
}  // namespace

TEST_CASE("effects: amount 0, None, unprepared scratch and bad sizes leave the frame untouched") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    const PixelBuffer ref = baseFrame();
    for (auto kind : {EffectKind::PrismMirror, EffectKind::BloomPulse}) {
        PixelBuffer f = ref;
        applyEffect(kind, 0.f, fx.in(), f, sc);
        CHECK(f.rgba == ref.rgba);
        applyEffect(kind, -2.f, fx.in(), f, sc);
        CHECK(f.rgba == ref.rgba);
        applyEffect(kind, std::nanf(""), fx.in(), f, sc);
        CHECK(f.rgba == ref.rgba);
        EffectScratch other;                                         // never prepared
        applyEffect(kind, 1.f, fx.in(), f, other);
        CHECK(f.rgba == ref.rgba);
        PixelBuffer small = baseFrame(80, 60);                       // scratch prepared for another size
        const auto smallRef = small.rgba;
        applyEffect(kind, 1.f, fx.in(), small, sc);
        CHECK(small.rgba == smallRef);
    }
    PixelBuffer f = ref;
    applyEffect(EffectKind::None, 1.f, fx.in(), f, sc);
    CHECK(f.rgba == ref.rgba);
    EffectScratch big;
    big.prepare(100000, 100000);                                     // rejected before allocation
    CHECK_FALSE(big.ready(100000, 100000));
    CHECK(big.bytes() == 0);
}

TEST_CASE("PrismMirror: deterministic, blends with amount, segments clamp, rotates with time") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    const PixelBuffer ref = baseFrame();
    auto run = [&](float amount, int segs, Ns t) {
        fx.params.prismSegments = segs;
        PixelBuffer f = ref;
        applyEffect(EffectKind::PrismMirror, amount, fx.in(t), f, sc);
        return f;
    };
    const PixelBuffer a = run(1.f, 6, 3'000'000'000LL), b = run(1.f, 6, 3'000'000'000LL);
    CHECK(a.rgba == b.rgba);                                         // deterministic
    CHECK(a.rgba != ref.rgba);
    const PixelBuffer half = run(0.5f, 6, 3'000'000'000LL);
    // 50% result lies between source and full effect per channel (+-1 rounding)
    bool between = true;
    for (std::size_t i = 0; i < ref.rgba.size(); ++i) {
        const int lo = std::min(ref.rgba[i], a.rgba[i]) - 1, hi = std::max(ref.rgba[i], a.rgba[i]) + 1;
        between &= half.rgba[i] >= lo && half.rgba[i] <= hi;
    }
    CHECK(between);
    CHECK(run(1.f, 1, 0).rgba == run(1.f, 2, 0).rgba);              // below range clamps to 2
    CHECK(run(1.f, 99, 0).rgba == run(1.f, 16, 0).rgba);            // above range clamps to 16
    CHECK(run(1.f, 3, 0).rgba != run(1.f, 4, 0).rgba);
    CHECK(run(1.f, 6, 0).rgba != run(1.f, 6, 20'000'000'000LL).rgba);   // slow rotation
    // slow: one second moves only a small fraction of pixels' wedge (rotation is 1 turn / 40 s)
    const PixelBuffer t0 = run(1.f, 6, 0), t1 = run(1.f, 6, 100'000'000LL);
    std::size_t diff = 0;
    for (std::size_t i = 0; i < t0.rgba.size(); i += 4) diff += (t0.rgba[i] != t1.rgba[i] || t0.rgba[i + 1] != t1.rgba[i + 1]);
    CHECK(diff < t0.rgba.size() / 4 * 3 / 4);
    CHECK(run(1.f, 6, 0).rgba.size() == ref.rgba.size());           // size preserved
    // alpha preserved on an opaque frame
    bool opaque = true;
    for (std::size_t i = 3; i < a.rgba.size(); i += 4) opaque &= a.rgba[i] == 255;
    CHECK(opaque);
}

TEST_CASE("PrismMirror: kaleidoscope symmetry on a left/right split frame (N=2, t=0)") {
    // With 2 segments the fold is symmetric about the horizontal axis through the centre: the output must have
    // far more top/bottom agreement than the asymmetric source.
    rt::Fixture fx;
    fx.params.prismSegments = 2;
    EffectScratch sc;
    sc.prepare(160, 120);
    PixelBuffer f(160, 120);
    for (std::uint32_t y = 0; y < 120; ++y)
        for (std::uint32_t x = 0; x < 160; ++x) {
            std::uint8_t* p = f.row(y) + x * 4;
            p[0] = static_cast<std::uint8_t>(x * 255 / 159); p[1] = static_cast<std::uint8_t>(y * 255 / 119); p[2] = 40; p[3] = 255;
        }
    auto mismatch = [](const PixelBuffer& b) {
        std::size_t bad = 0, n = 0;
        for (std::uint32_t y = 0; y < b.height / 2; ++y)
            for (std::uint32_t x = 0; x < b.width; ++x) {
                const std::uint8_t* t = b.row(y) + x * 4;
                const std::uint8_t* m = b.row(b.height - 1 - y) + x * 4;
                bad += (std::abs(int(t[0]) - m[0]) + std::abs(int(t[1]) - m[1])) > 24;
                ++n;
            }
        return double(bad) / double(n);
    };
    const double before = mismatch(f);
    applyEffect(EffectKind::PrismMirror, 1.f, fx.in(0), f, sc);
    const double after = mismatch(f);
    CHECK(before > 0.8);
    CHECK(after < 0.25);
}

TEST_CASE("BloomPulse: glows around bright pixels, follows onset/low band, bounded and premultiplied-safe") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    const PixelBuffer ref = dotFrame(160, 120);
    auto run = [&](float amount, float low, float onset, bool strobeOff) {
        fx.feat.band[0] = low; fx.feat.onsetStrength = onset; fx.feat.onsetFired = false;
        auto in = fx.in(); in.strobingDisabled = strobeOff;
        PixelBuffer f = ref;
        applyEffect(EffectKind::BloomPulse, amount, in, f, sc);
        return f;
    };
    auto sumRegion = [](const PixelBuffer& b, int r0) {   // total of channel 0 outside the 16x16 block, within r0 px
        long long s = 0;
        for (int y = 0; y < int(b.height); ++y)
            for (int x = 0; x < int(b.width); ++x) {
                const int dx = x - int(b.width / 2), dy = y - int(b.height / 2);
                if (dx >= -8 && dx < 8 && dy >= -8 && dy < 8) continue;
                if (std::abs(dx) <= r0 && std::abs(dy) <= r0) s += b.row(y)[x * 4];
            }
        return s;
    };
    const PixelBuffer off = run(1.f, 0.f, 0.f, false), loud = run(1.f, 1.f, 1.f, false);
    CHECK(sumRegion(ref, 40) == 0);
    CHECK(sumRegion(off, 40) > 0);                                   // glow exists
    CHECK(sumRegion(loud, 40) > sumRegion(off, 40));                 // stronger on onset + low band
    CHECK(run(1.f, 1.f, 1.f, false).rgba == loud.rgba);              // deterministic
    CHECK(sumRegion(run(0.25f, 1.f, 1.f, false), 40) < sumRegion(loud, 40));   // amount scales it
    CHECK(sumRegion(run(1.f, 1.f, 1.f, true), 40) < sumRegion(loud, 40));      // strobing disabled narrows the pulse
    // far corners stay dark (bounded radius), centre dot never darkened, colour never above alpha
    CHECK(loud.row(2)[2 * 4] == 0);
    CHECK(loud.row(60)[80 * 4] == 255);
    bool safe = true;
    for (std::size_t i = 0; i < loud.rgba.size(); i += 4) safe &= loud.rgba[i] <= loud.rgba[i + 3] && loud.rgba[i + 1] <= loud.rgba[i + 3];
    CHECK(safe);
    // partially transparent premultiplied input: colour is clamped to alpha
    PixelBuffer half = ref;
    for (std::size_t i = 0; i < half.rgba.size(); i += 4) { half.rgba[i + 3] = 100; if (half.rgba[i] > 100) half.rgba[i] = half.rgba[i + 1] = half.rgba[i + 2] = 100; }
    fx.feat.band[0] = 1.f; fx.feat.onsetStrength = 1.f;
    applyEffect(EffectKind::BloomPulse, 1.f, fx.in(), half, sc);
    bool clamped = true;
    for (std::size_t i = 0; i < half.rgba.size(); i += 4) clamped &= half.rgba[i] <= 100 && half.rgba[i + 3] == 100;
    CHECK(clamped);
}

TEST_CASE("effects: guard canaries, odd sizes, null inputs") {
    const std::uint32_t sizes[][2] = {{1, 1}, {2, 3}, {5, 7}, {17, 5}, {64, 33}, {161, 121}};
    for (auto kind : {EffectKind::PrismMirror, EffectKind::BloomPulse})
        for (auto& sz : sizes) {
            EffectScratch sc;
            sc.prepare(sz[0], sz[1]);
            rt::Fixture fx;
            PixelBuffer f = rt::renderOnce(SceneKind::PlasmaLite, sz[0], sz[1], fx.in());
            const std::size_t bytes = f.rgba.size();
            f.rgba.resize(bytes + 256, 0xAB);
            applyEffect(kind, 1.f, fx.in(), f, sc);
            SceneInput bare;                                         // null features/params
            applyEffect(kind, 0.7f, bare, f, sc);
            for (std::size_t i = bytes; i < f.rgba.size(); ++i) REQUIRE(f.rgba[i] == 0xAB);
        }
}

TEST_CASE("effects: golden frames and zero allocations in the hot path (400x300 internal frame)") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    {
        fx.params.prismSegments = 6;
        PixelBuffer f = baseFrame();
        applyEffect(EffectKind::PrismMirror, 0.75f, fx.in(), f, sc);
        rt::checkGolden("effect.PrismMirror.6", rt::hashOf(f));
        fx.params.prismSegments = 5;
        f = baseFrame();
        applyEffect(EffectKind::PrismMirror, 1.f, fx.in(), f, sc);
        rt::checkGolden("effect.PrismMirror.5", rt::hashOf(f));
        fx.feat.onsetStrength = 0.5f;
        f = baseFrame();
        applyEffect(EffectKind::BloomPulse, 1.f, fx.in(), f, sc);
        rt::checkGolden("effect.BloomPulse", rt::hashOf(f));
        f = dotFrame(160, 120);
        applyEffect(EffectKind::BloomPulse, 0.5f, fx.in(), f, sc);
        rt::checkGolden("effect.BloomPulse.dot", rt::hashOf(f));
    }
    EffectScratch big;
    big.prepare(400, 300);
    CHECK(big.bytes() < 12u * 1024 * 1024);                          // inside the BASE 12 MiB frame-pool allowance
    PixelBuffer f = rt::renderOnce(SceneKind::NeonGrid, 400, 300, fx.in());
    std::size_t n;
    {
        alloc_probe::Scope scope;
        applyEffect(EffectKind::PrismMirror, 1.f, fx.in(), f, big);
        applyEffect(EffectKind::BloomPulse, 1.f, fx.in(), f, big);
        applyEffect(EffectKind::BloomPulse, 0.3f, SceneInput{}, f, big);
        n = scope.n();
    }
    CHECK(n == 0);
}

TEST_CASE("benchmark (prints only): effect ms per frame at 320x240") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(320, 240);
    PixelBuffer f = rt::renderOnce(SceneKind::PlasmaLite, 320, 240, fx.in());
    std::printf("\n[render bench] effects 320x240\n");
    for (auto kind : {EffectKind::PrismMirror, EffectKind::BloomPulse}) {
        applyEffect(kind, 1.f, fx.in(), f, sc);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) applyEffect(kind, 1.f, fx.in(i * 40'000'000LL), f, sc);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30;
        std::printf("  %-12s %7.3f ms\n", kind == EffectKind::PrismMirror ? "PrismMirror" : "BloomPulse", ms);
    }
    std::fflush(stdout);
    CHECK(true);
}

// ---- NEW-02 acceptance 2: WCAG 2.3.1 general-flash bound, measured on a luminance trace --------------------------
namespace {
double relLuminance(const PixelBuffer& b) {
    auto lin = [](double c) { c /= 255.0; return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    double s = 0.0;
    for (std::size_t i = 0; i < b.rgba.size(); i += 4)
        s += 0.2126 * lin(b.rgba[i]) + 0.7152 * lin(b.rgba[i + 1]) + 0.0722 * lin(b.rgba[i + 2]);
    return s / double(b.rgba.size() / 4);
}
// WCAG general flash: a pair of opposing changes in relative luminance of >= 0.10 where the darker state is < 0.80.
// Counts flashes (opposing pairs) in the worst 1 s window of a trace sampled at `fps`.
int worstFlashesPerSecond(const std::vector<double>& trace, int fps) {
    std::vector<int> changeAt;             // frame index of each qualifying change, with its sign
    std::vector<int> sign;
    for (std::size_t i = 1; i < trace.size(); ++i) {
        const double d = trace[i] - trace[i - 1];
        if (std::fabs(d) >= 0.10 && std::min(trace[i], trace[i - 1]) < 0.80) { changeAt.push_back(int(i)); sign.push_back(d > 0 ? 1 : -1); }
    }
    int worst = 0;
    for (std::size_t a = 0; a < changeAt.size(); ++a) {
        int flashes = 0, last = 0;
        for (std::size_t b = a; b < changeAt.size() && changeAt[b] - changeAt[a] < fps; ++b)
            if (last != 0 && sign[b] == -last) { ++flashes; last = 0; } else if (last == 0 || sign[b] == last) last = sign[b];
        worst = std::max(worst, flashes);
    }
    return worst;
}
PixelBuffer solidFrame(std::uint32_t w, std::uint32_t h, std::uint8_t v, bool leftHalfOnly = false) {
    PixelBuffer b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.row(y) + x * 4;
            const bool on = !leftHalfOnly || x < w / 2;
            p[0] = p[1] = p[2] = on ? v : 0; p[3] = 255;
        }
    return b;
}
}  // namespace

TEST_CASE("NEW-02: flash counter flags a 6 Hz black/white strobe and ignores a steady glow") {
    std::vector<double> strobe, steady;
    for (int i = 0; i < 60; ++i) { strobe.push_back((i / 2) % 2 ? 1.0 : 0.0); steady.push_back(0.4 + 0.01 * (i % 2)); }
    CHECK(worstFlashesPerSecond(strobe, 30) > 3);
    CHECK(worstFlashesPerSecond(steady, 30) == 0);
}

TEST_CASE("NEW-02: bloom pulse stays inside the general-flash bound, driven by an onset on every frame at 30, 60 and 120 fps") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    const PixelBuffer frames[] = {dotFrame(160, 120), solidFrame(160, 120, 255, true), solidFrame(160, 120, 120), solidFrame(160, 120, 200),
                                  solidFrame(160, 120, 97), baseFrame()};
    int idx = 0;
    for (const PixelBuffer& ref : frames) {
        ++idx;
        for (int fps : {30, 60, 120})
            for (bool locked : {false, true}) {
                CAPTURE(idx); CAPTURE(fps); CAPTURE(locked);
                sc.prepare(160, 120);                                // fresh state per trace
                std::vector<double> trace;
                for (int frame = 0; frame < 3 * fps; ++frame) {      // 3 s; worst-case drive alternates every frame
                    const bool hit = frame % 2 == 0;
                    fx.feat.band[0] = hit ? 1.f : 0.f; fx.feat.onsetStrength = hit ? 1.f : 0.f; fx.feat.onsetFired = hit;
                    auto in = fx.in(Ns(1'000'000'000LL) + Ns(frame) * 1'000'000'000LL / fps);
                    in.strobingDisabled = locked;
                    PixelBuffer f = ref;
                    applyEffect(EffectKind::BloomPulse, 1.f, in, f, sc);
                    trace.push_back(relLuminance(f));
                }
                CHECK(worstFlashesPerSecond(trace, fps) <= 3);       // acceptance 2
                std::vector<double> coarse;                          // the same trace seen at 10 Hz: a slow-camera / coarse check
                for (std::size_t i = 0; i < trace.size(); i += std::size_t(fps / 10)) coarse.push_back(trace[i]);
                CHECK(worstFlashesPerSecond(coarse, 10) <= 3);
                if (locked) {                                        // Disable Strobing: no crossing of the 0.10 threshold at all
                    CHECK(worstFlashesPerSecond(trace, fps) == 0);
                    const auto mm = std::minmax_element(trace.begin(), trace.end());
                    CHECK(*mm.second - *mm.first < 0.10);
                }
            }
    }
}

TEST_CASE("NEW-02: bloom pulse rises with a bounded attack and falls with a bounded release") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    const PixelBuffer ref = solidFrame(160, 120, 255, true);
    auto lumAt = [&](Ns t, bool hit) {
        fx.feat.band[0] = hit ? 1.f : 0.f; fx.feat.onsetStrength = hit ? 1.f : 0.f; fx.feat.onsetFired = hit;
        PixelBuffer f = ref;
        applyEffect(EffectKind::BloomPulse, 1.f, fx.in(t), f, sc);
        return relLuminance(f);
    };
    const double quiet = lumAt(1'000'000'000LL, false);                       // seeds at the quiet level
    const double afterHit = lumAt(1'000'000'000LL + 33'000'000LL, true);      // one 33 ms frame after a full-strength onset
    const double settled = lumAt(2'000'000'000LL, true);                      // 1 s later: fully risen
    CHECK(afterHit > quiet);
    CHECK(afterHit - quiet < 0.5 * (settled - quiet));                        // one frame covers only part of the swing
    const double falling = lumAt(2'000'000'000LL + 33'000'000LL, false);
    CHECK(falling < settled);
    CHECK(falling > 0.5 * (settled + quiet));                                 // release is slower than one frame
    CHECK(lumAt(4'000'000'000LL, false) == doctest::Approx(quiet));           // and does return to the quiet level
}

TEST_CASE("NEW-02: amount 0 is a bit-exact pass-through and output never exceeds alpha (no clipping artefacts)") {
    rt::Fixture fx;
    EffectScratch sc;
    sc.prepare(160, 120);
    for (const PixelBuffer& ref : {dotFrame(160, 120), solidFrame(160, 120, 255)}) {
        PixelBuffer off = ref;
        applyEffect(EffectKind::BloomPulse, 0.f, fx.in(), off, sc);
        CHECK(off.rgba == ref.rgba);
        fx.feat.band[0] = 1.f; fx.feat.onsetStrength = 1.f; fx.feat.onsetFired = true;
        PixelBuffer on = ref;
        applyEffect(EffectKind::BloomPulse, 1.f, fx.in(), on, sc);
        for (std::size_t i = 0; i < on.rgba.size(); i += 4) {
            REQUIRE(on.rgba[i] <= on.rgba[i + 3]);
            REQUIRE(on.rgba[i] >= ref.rgba[i]);                  // glow only adds
        }
    }
}
