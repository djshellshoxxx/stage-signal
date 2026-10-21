#include <chrono>
#include <cmath>
#include <set>
#include "test_util.hpp"

using namespace ss;
using namespace ss::render;
using ss::model::SceneKind;

namespace {
std::string kindName(SceneKind k) { return makeScene(k)->name(); }
bool isStrobeKind(SceneKind k) { return k == SceneKind::Starburst || k == SceneKind::ColorField; }
}  // namespace

TEST_CASE("REQ-D07: factory builds the 12 procedural scenes and rejects media kinds") {
    std::set<std::string> names;
    for (auto k : rt::kAllKinds) {
        auto s = makeScene(k);
        REQUIRE(s != nullptr);
        CHECK(std::string(s->name()).size() > 3);
        CHECK(s->costEstimate() > 0.f);
        CHECK(s->costEstimate() < 4.f);
        names.insert(s->name());
    }
    CHECK(names.size() == 12);
    CHECK(makeScene(SceneKind::Image) == nullptr);
    CHECK(makeScene(SceneKind::Gif) == nullptr);
    CHECK(makeScene(SceneKind::Video) == nullptr);
    CHECK(makeScene(SceneKind::Count) == nullptr);
}

TEST_CASE("golden frames: 160x120, fixed seed/time/audio, hash matches committed value") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) {
        SUBCASE(kindName(k).c_str()) {
            const auto f = rt::renderOnce(k, 160, 120, fx.in());
            rt::checkGolden(kindName(k), rt::hashOf(f));
        }
    }
}

TEST_CASE("golden frames: strobe scenes, flash event with strobing on and off") {
    rt::Fixture fx;
    fx.feat.onsetFired = true; fx.feat.onsetStrength = 1.f;
    for (auto k : {SceneKind::Starburst, SceneKind::ColorField}) {
        auto on = fx.in();
        auto off = fx.in(); off.strobingDisabled = true;
        rt::checkGolden(kindName(k) + ".flash", rt::hashOf(rt::renderOnce(k, 160, 120, on)));
        rt::checkGolden(kindName(k) + ".flash.nostrobe", rt::hashOf(rt::renderOnce(k, 160, 120, off)));
    }
}

TEST_CASE("determinism: same inputs render identically twice, across instances, and after frame skips") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        auto a = makeScene(k), b = makeScene(k);
        a->prepare(160, 120, 77); b->prepare(160, 120, 77);
        PixelBuffer o1(160, 120), o2(160, 120), o3(160, 120);
        a->render(fx.in(5'000'000'000LL), o1);
        a->render(fx.in(5'000'000'000LL), o2);                    // same instance again
        for (int i = 0; i < 7; ++i) b->render(fx.in(i * 40'000'000LL), o3);   // other instance, earlier frames first
        b->render(fx.in(5'000'000'000LL), o3);
        CHECK(rt::hashOf(o1) == rt::hashOf(o2));
        CHECK(rt::hashOf(o1) == rt::hashOf(o3));
    }
}

TEST_CASE("animation derives from elapsed time: speed scales it, speed 0 freezes it") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        const auto a = rt::renderOnce(k, 160, 120, fx.in(1'000'000'000LL)), b = rt::renderOnce(k, 160, 120, fx.in(1'700'000'000LL));
        // the two bar scenes are pure functions of the audio bins (no time term) while audio is supplied
        if (k != SceneKind::SpectrumBars && k != SceneKind::MirrorBars) CHECK(rt::hashOf(a) != rt::hashOf(b));
        fx.params.speed = 0.f;
        const auto c = rt::renderOnce(k, 160, 120, fx.in(1'000'000'000LL)), d = rt::renderOnce(k, 160, 120, fx.in(9'000'000'000LL));
        CHECK(rt::hashOf(c) == rt::hashOf(d));
        fx.params.speed = 1.5f;
        // no audio, no features, no params: idle animation still moves and is non-black
        SceneInput idle; idle.timeNs = 1'000'000'000LL;
        SceneInput idle2; idle2.timeNs = 1'900'000'000LL;
        const auto e = rt::renderOnce(k, 160, 120, idle), f = rt::renderOnce(k, 160, 120, idle2);
        CHECK_FALSE(rt::allBlack(e));
        CHECK(rt::hashOf(e) != rt::hashOf(f));
    }
}

TEST_CASE("property: frames are non-black, opaque, and brightness 0 gives black") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        const auto f = rt::renderOnce(k, 160, 120, fx.in());
        CHECK_FALSE(rt::allBlack(f));
        bool opaque = true;
        for (std::size_t i = 3; i < f.rgba.size(); i += 4) opaque &= f.rgba[i] == 255;
        CHECK(opaque);
        fx.params.brightness = 0.f;
        CHECK(rt::allBlack(rt::renderOnce(k, 160, 120, fx.in())));
        fx.params.brightness = -3.f;                               // out of range clamps to black
        CHECK(rt::allBlack(rt::renderOnce(k, 160, 120, fx.in())));
        fx.params.brightness = 0.75f;
    }
}

TEST_CASE("property: parameters take effect (hue, saturation 0 is grey, brightness is monotonic)") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        const auto base = rt::renderOnce(k, 160, 120, fx.in());
        fx.params.hue = 0.6f;
        CHECK(rt::hashOf(rt::renderOnce(k, 160, 120, fx.in())) != rt::hashOf(base));
        fx.params.hue = 0.25f;
        fx.params.saturation = 0.f;
        const auto grey = rt::renderOnce(k, 160, 120, fx.in());
        bool isGrey = true;
        for (std::size_t i = 0; i < grey.rgba.size(); i += 4) isGrey &= (grey.rgba[i] == grey.rgba[i + 1] && grey.rgba[i] == grey.rgba[i + 2]);
        CHECK(isGrey);
        fx.params.saturation = 0.875f;
        fx.params.brightness = 0.25f;
        const double dim = rt::meanLuma(rt::renderOnce(k, 160, 120, fx.in()));
        fx.params.brightness = 1.f;
        const double bright = rt::meanLuma(rt::renderOnce(k, 160, 120, fx.in()));
        CHECK(bright > dim);
        fx.params.brightness = 0.75f;
        fx.params.intensity = 1.f;
        const auto hi = rt::renderOnce(k, 160, 120, fx.in());
        fx.params.intensity = 0.f;
        const auto lo = rt::renderOnce(k, 160, 120, fx.in());
        CHECK(rt::hashOf(hi) != rt::hashOf(lo));
        fx.params.intensity = 0.625f;
    }
}

TEST_CASE("property: out-of-range and non-finite parameters never crash or escape the buffer") {
    rt::Fixture fx;
    const float bad[] = {-1e30f, 1e30f, std::nanf(""), INFINITY, -INFINITY, 0.f, 2.f};
    for (auto k : rt::kAllKinds)
        for (float v : bad) {
            fx.params.brightness = fx.params.speed = fx.params.intensity = fx.params.hue = fx.params.saturation = v;
            fx.audio.spectrum[3] = v; fx.audio.waveform[9] = v; fx.feat.band[0] = v; fx.feat.onsetStrength = v;
            auto s = makeScene(k);
            s->prepare(64, 48, 1);
            PixelBuffer out(64, 48);
            out.rgba.resize(64 * 48 * 4 + 64, 0xAB);
            s->render(fx.in(), out);
            for (std::size_t i = 64 * 48 * 4; i < out.rgba.size(); ++i) REQUIRE(out.rgba[i] == 0xAB);
        }
}

TEST_CASE("property: no out-of-range writes (guard canaries) for many frame sizes") {
    rt::Fixture fx;
    const std::uint32_t sizes[][2] = {{1, 1}, {2, 3}, {17, 5}, {33, 64}, {5, 200}, {160, 120}, {251, 97}};
    for (auto k : rt::kAllKinds)
        for (auto& sz : sizes) {
            INFO(kindName(k) << " " << sz[0] << "x" << sz[1]);
            auto s = makeScene(k);
            s->prepare(sz[0], sz[1], 5);
            const std::size_t bytes = std::size_t(sz[0]) * sz[1] * 4;
            PixelBuffer out(sz[0], sz[1]);
            out.rgba.resize(bytes + 512, 0xAB);                    // 512 canary bytes behind the frame
            for (int pass = 0; pass < 3; ++pass) {
                fx.feat.onsetFired = pass == 1;
                s->render(fx.in(pass * 333'000'000LL), out);
                for (std::size_t i = bytes; i < out.rgba.size(); ++i) REQUIRE(out.rgba[i] == 0xAB);
            }
            // output of the wrong size is cleared, never overrun
            PixelBuffer wrong(sz[0] + 3, sz[1] + 1);
            wrong.rgba.resize(wrong.rgba.size() + 64, 0xAB);
            s->render(fx.in(), wrong);
            for (std::size_t i = std::size_t(wrong.width) * wrong.height * 4; i < wrong.rgba.size(); ++i) REQUIRE(wrong.rgba[i] == 0xAB);
            PixelBuffer empty;
            s->render(fx.in(), empty);                              // must not crash
        }
}

TEST_CASE("REQ-S12: only flashing scenes report strobe, and they have a non-flashing fallback") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) CHECK(makeScene(k)->usesStrobe() == isStrobeKind(k));

    rt::Fixture fxHit = fx;
    fxHit.feat.onsetFired = true; fxHit.feat.onsetStrength = 1.f;
    SceneInput quiet = fx.in();
    SceneInput hit = fxHit.in();                                    // an onset event at this frame
    SceneInput hitSafe = hit; hitSafe.strobingDisabled = true;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        auto sOn = makeScene(k), sOff = makeScene(k), sBase = makeScene(k);
        for (auto* s : {sOn.get(), sOff.get(), sBase.get()}) s->prepare(160, 120, 9);
        PixelBuffer base(160, 120), on(160, 120), off(160, 120);
        sBase->render(quiet, base);
        sOn->render(hit, on);
        sOff->render(hitSafe, off);
        if (isStrobeKind(k)) {
            CHECK(rt::hashOf(on) != rt::hashOf(off));              // fallback differs from the flash
            const double flashJump = rt::meanLuma(on) - rt::meanLuma(base);
            const double safeJump = rt::meanLuma(off) - rt::meanLuma(base);
            CHECK(flashJump > 20.0);                               // a real full-frame flash
            CHECK(safeJump < 0.07 * 255.0);                        // fallback stays under ~7% of full scale
            CHECK(safeJump < flashJump * 0.5);
        } else {
            CHECK(rt::hashOf(on) == rt::hashOf(off));              // non-strobing scenes ignore the switch
        }
    }
}

TEST_CASE("REQ-S12: fallback holds for the whole flash decay window, frame to frame") {
    rt::Fixture fx;
    for (auto k : {SceneKind::Starburst, SceneKind::ColorField}) {
        INFO(kindName(k));
        auto s = makeScene(k);
        s->prepare(160, 120, 3);
        PixelBuffer a(160, 120);
        double prev = 0, worst = 0;
        for (int i = 0; i < 20; ++i) {
            fx.feat.onsetFired = (i == 4);
            fx.feat.onsetStrength = i == 4 ? 1.f : 0.5f;
            auto in = fx.in(1'000'000'000LL + i * 25'000'000LL);
            in.strobingDisabled = true;
            s->render(in, a);
            const double l = rt::meanLuma(a);
            if (i) worst = std::max(worst, std::fabs(l - prev));
            prev = l;
        }
        CHECK(worst < 0.07 * 255.0);
    }
}

TEST_CASE("audio reactivity: loud audio+features differ from silence") {
    rt::Fixture fx, silent;
    for (int i = 0; i < 32; ++i) silent.audio.spectrum[i] = 0.f;
    for (int i = 0; i < 256; ++i) silent.audio.waveform[i] = 0.f;
    silent.feat.band[0] = silent.feat.band[1] = silent.feat.band[2] = 0.f;
    silent.feat.onsetStrength = 0.f;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        CHECK(rt::hashOf(rt::renderOnce(k, 160, 120, fx.in())) != rt::hashOf(rt::renderOnce(k, 160, 120, silent.in())));
        CHECK_FALSE(rt::allBlack(rt::renderOnce(k, 160, 120, silent.in())));   // silence still draws something
    }
    // spectrum-driven scenes respond to the spectrum alone
    for (auto k : {SceneKind::SpectrumBars, SceneKind::MirrorBars}) {
        rt::Fixture a = fx; a.audio.spectrum[5] = 1.f;
        rt::Fixture b = fx; b.audio.spectrum[5] = 0.f;
        CHECK(rt::hashOf(rt::renderOnce(k, 160, 120, a.in())) != rt::hashOf(rt::renderOnce(k, 160, 120, b.in())));
    }
    for (auto k : {SceneKind::WaveformRibbon, SceneKind::LissajousScope}) {
        rt::Fixture a = fx; a.audio.waveform[100] = 1.f;
        rt::Fixture b = fx; b.audio.waveform[100] = -1.f;
        for (int i = 96; i < 106; ++i) { a.audio.waveform[i] = 1.f; b.audio.waveform[i] = -1.f; }
        CHECK(rt::hashOf(rt::renderOnce(k, 160, 120, a.in())) != rt::hashOf(rt::renderOnce(k, 160, 120, b.in())));
    }
}

TEST_CASE("seeded state: seed changes content for seeded scenes, in.seed reseeds like prepare(seed)") {
    rt::Fixture fx;
    for (auto k : {SceneKind::ParticleField, SceneKind::GeometricOrbit, SceneKind::Tunnel, SceneKind::Starburst, SceneKind::PlasmaLite}) {
        INFO(kindName(k));
        auto keep = fx.in(); keep.seed = 0;
        const auto a = rt::renderOnce(k, 160, 120, keep, 1), b = rt::renderOnce(k, 160, 120, keep, 2);
        CHECK(rt::hashOf(a) != rt::hashOf(b));
        auto in = fx.in(); in.seed = 2;                             // reseed through SceneInput
        CHECK(rt::hashOf(rt::renderOnce(k, 160, 120, in, 1)) == rt::hashOf(b));
        in.seed = 0;                                                // 0 = keep the prepared seed
    }
}

TEST_CASE("REQ-P06: render() performs zero heap allocations after prepare()") {
    rt::Fixture fx;
    for (auto k : rt::kAllKinds) {
        INFO(kindName(k));
        auto s = makeScene(k);
        s->prepare(400, 300, 11);
        PixelBuffer out(400, 300);
        s->render(fx.in(), out);                                    // warm
        std::size_t n;
        {
            alloc_probe::Scope scope;
            for (int i = 0; i < 4; ++i) {
                fx.feat.onsetFired = (i & 1) != 0;
                auto in = fx.in(i * 41'000'000LL);
                in.strobingDisabled = (i == 3);
                s->render(in, out);
            }
            SceneInput bare;                                         // all pointers null
            bare.timeNs = 12345;
            s->render(bare, out);
            n = scope.n();
        }
        CHECK(n == 0);
    }
}

TEST_CASE("benchmark (prints only): ms per frame at 320x240") {
    rt::Fixture fx;
    std::printf("\n[render bench] 320x240, ms/frame (informational, no assertion)\n");
    for (auto k : rt::kAllKinds) {
        auto s = makeScene(k);
        s->prepare(320, 240, 1);
        PixelBuffer out(320, 240);
        s->render(fx.in(), out);
        constexpr int frames = 30;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) s->render(fx.in(i * 40'000'000LL), out);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
        std::printf("  %-15s %7.3f ms   (costEstimate %.2f)\n", s->name(), ms, s->costEstimate());
    }
    std::fflush(stdout);
    CHECK(true);
}
