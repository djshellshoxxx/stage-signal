#include "util.hpp"
using namespace tu;
using TK = ss::model::TransitionKind;

TEST_CASE("transition: crossfade exact goldens at 16x12") {
    const PixelBuffer a = solid(16, 12, Px{255, 0, 0, 255}), b = solid(16, 12, Px{0, 0, 255, 255});
    PixelBuffer out;
    REQUIRE(blendTransition(a, b, TK::Crossfade, 0.f, out));
    CHECK(allEqual(out, Px{255, 0, 0, 255}));          // progress 0 == A bit-exact
    REQUIRE(blendTransition(a, b, TK::Crossfade, 1.f, out));
    CHECK(allEqual(out, Px{0, 0, 255, 255}));          // progress 1 == B bit-exact
    REQUIRE(blendTransition(a, b, TK::Crossfade, 0.5f, out));
    CHECK(allEqual(out, Px{127, 0, 128, 255}));        // weight 128/255
    REQUIRE(blendTransition(a, b, TK::Crossfade, -3.f, out));
    CHECK(allEqual(out, Px{255, 0, 0, 255}));
    REQUIRE(blendTransition(a, b, TK::Crossfade, 7.f, out));
    CHECK(allEqual(out, Px{0, 0, 255, 255}));
    REQUIRE(blendTransition(a, b, TK::Crossfade, std::nanf(""), out));
    CHECK(allEqual(out, Px{255, 0, 0, 255}));
}

TEST_CASE("transition: crossfade is monotonic and never overflows with translucent premultiplied input") {
    const PixelBuffer a = solid(4, 4, Px{128, 0, 0, 128}), b = solid(4, 4, Px{0, 200, 0, 200});
    int prevR = 256, prevG = -1;
    for (int i = 0; i <= 100; ++i) {
        PixelBuffer out;
        REQUIRE(blendTransition(a, b, TK::Crossfade, i / 100.f, out));
        const Px p = at(out, 1, 1);
        CHECK(p.r <= p.a);
        CHECK(p.g <= p.a);
        CHECK(int(p.r) <= prevR);
        CHECK(int(p.g) >= prevG);
        prevR = p.r;
        prevG = p.g;
    }
}

TEST_CASE("transition: fade-through-black exact goldens, reaches pure black at 0.5, alpha untouched") {
    const PixelBuffer a = solid(16, 12, Px{255, 0, 0, 255}), b = solid(16, 12, Px{0, 0, 255, 255});
    PixelBuffer out;
    REQUIRE(blendTransition(a, b, TK::FadeThroughBlack, 0.f, out));
    CHECK(allEqual(out, Px{255, 0, 0, 255}));
    REQUIRE(blendTransition(a, b, TK::FadeThroughBlack, 0.25f, out));
    CHECK(allEqual(out, Px{127, 0, 0, 255}));
    REQUIRE(blendTransition(a, b, TK::FadeThroughBlack, 0.5f, out));
    CHECK(allEqual(out, Px{0, 0, 0, 255}));
    REQUIRE(blendTransition(a, b, TK::FadeThroughBlack, 0.75f, out));
    CHECK(allEqual(out, Px{0, 0, 128, 255}));
    REQUIRE(blendTransition(a, b, TK::FadeThroughBlack, 1.f, out));
    CHECK(allEqual(out, Px{0, 0, 255, 255}));
}

TEST_CASE("transition: cut switches on first progress, size mismatch is refused") {
    const PixelBuffer a = solid(4, 4, Px{1, 1, 1, 255}), b = solid(4, 4, Px{9, 9, 9, 255});
    PixelBuffer out;
    REQUIRE(blendTransition(a, b, TK::Cut, 0.f, out));
    CHECK(allEqual(out, Px{1, 1, 1, 255}));
    REQUIRE(blendTransition(a, b, TK::Cut, 0.01f, out));
    CHECK(allEqual(out, Px{9, 9, 9, 255}));
    const PixelBuffer c = solid(5, 4, Px{0, 0, 0, 255});
    CHECK(!blendTransition(a, c, TK::Crossfade, 0.5f, out));
}

TEST_CASE("transition: compositor drives it from two scene buffers (scene A/B of different sizes)") {
    Compositor c;
    REQUIRE(c.prepare(16, 12).ok());
    const PixelBuffer a = solid(8, 6, Px{255, 0, 0, 255}), b = solid(32, 24, Px{0, 0, 255, 255});
    FrameInputs in = sceneOnly(&a);
    in.scene.b = &b;
    in.scene.kind = TK::Crossfade;
    in.scene.progress = 0.5f;
    ComposedFrame f = c.compose(in);
    REQUIRE(f.ok());
    CHECK(allEqual(*f.pixels, Px{127, 0, 128, 255}));
    c.release(f);
    in.scene.kind = TK::FadeThroughBlack;
    f = c.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 0, 0, 255}));
    c.release(f);
    in.scene.progress = 1.f;
    f = c.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 0, 255, 255}));
    c.release(f);
}

TEST_CASE("REQ-S/section13: zero and sub-20 ms GIF delays clamp to 20 ms with a warning flag") {
    CHECK(clampGifDelay(0).effective == msToNs(20));
    CHECK(clampGifDelay(0).clamped);
    CHECK(clampGifDelay(msToNs(19)).clamped);
    CHECK(clampGifDelay(msToNs(10)).effective == msToNs(20));
    CHECK(!clampGifDelay(msToNs(20)).clamped);
    CHECK(clampGifDelay(msToNs(100)).effective == msToNs(100));
    CHECK(!clampGifDelay(msToNs(100)).clamped);
}

TEST_CASE("media timing: GIF frame selection follows the media clock, clamps, loops and speed") {
    auto r = GifTimeline::create({msToNs(100), 0, msToNs(50)});
    REQUIRE(r.ok());
    const GifTimeline& g = r.value();
    CHECK(g.anyDelayClamped());
    CHECK(g.totalDuration() == msToNs(170));
    CHECK(g.at(0).frame == 0);
    CHECK(g.at(msToNs(99)).frame == 0);
    CHECK(g.at(msToNs(100)).frame == 1);
    CHECK(g.at(msToNs(119)).frame == 1);
    CHECK(g.at(msToNs(120)).frame == 2);
    CHECK(g.at(msToNs(169)).frame == 2);
    CHECK(g.at(msToNs(170)).frame == 0);
    CHECK(g.at(msToNs(170)).loop == 1);
    CHECK(g.at(-5).frame == 0);
    // pure function of elapsed time: skipping render ticks does not change the answer
    CHECK(g.at(msToNs(1000)).frame == g.at(msToNs(1000)).frame);
    CHECK(g.at(msToNs(170) * 1000 + msToNs(120)).frame == 2);

    auto fast = GifTimeline::create({msToNs(100), msToNs(100)}, 2.0);
    REQUIRE(fast.ok());
    CHECK(fast.value().at(msToNs(49)).frame == 0);
    CHECK(fast.value().at(msToNs(50)).frame == 1);
    CHECK(GifTimeline::create({msToNs(100)}, 10.0).value().speed() == doctest::Approx(4.0));
    CHECK(GifTimeline::create({msToNs(100)}, 0.01).value().speed() == doctest::Approx(0.25));
    CHECK(GifTimeline::create({msToNs(100)}, std::nan("")).value().speed() == doctest::Approx(1.0));

    auto once = GifTimeline::create({msToNs(100), msToNs(100)}, 1.0, 1);
    REQUIRE(once.ok());
    CHECK(!once.value().at(msToNs(199)).finished);
    CHECK(once.value().at(msToNs(200)).finished);
    CHECK(once.value().at(msToNs(5000)).frame == 1);

    CHECK(!GifTimeline::create({}).ok());
    CHECK(!GifTimeline::create(std::vector<ss::Ns>(1001, msToNs(20))).ok());
    CHECK(GifTimeline::create(std::vector<ss::Ns>(1000, msToNs(20))).ok());
}

TEST_CASE("media timing: video position, speed bounds 0.25..2 and loop") {
    auto v = VideoTimeline::create(secToNs(10), 1.0, false);
    REQUIRE(v.ok());
    CHECK(v.value().at(secToNs(3)).mediaPts == secToNs(3));
    CHECK(!v.value().at(secToNs(9)).finished);
    CHECK(v.value().at(secToNs(10)).finished);
    CHECK(v.value().at(secToNs(99)).mediaPts == secToNs(10));
    auto fast = VideoTimeline::create(secToNs(10), 5.0, false);   // clamps to 2x
    CHECK(fast.value().at(secToNs(3)).mediaPts == secToNs(6));
    auto slow = VideoTimeline::create(secToNs(10), 0.0, false);   // clamps to 0.25x
    CHECK(slow.value().at(secToNs(4)).mediaPts == secToNs(1));
    auto lp = VideoTimeline::create(secToNs(10), 1.0, true);
    CHECK(lp.value().at(secToNs(25)).mediaPts == secToNs(5));
    CHECK(lp.value().at(secToNs(25)).loop == 2);
    CHECK(!lp.value().at(secToNs(25)).finished);
    CHECK(!VideoTimeline::create(0).ok());
}

TEST_CASE("media layers: crop, region, opacity and order above the scene") {
    Compositor c;
    REQUIRE(c.prepare(16, 12).ok());
    const PixelBuffer scene = solid(16, 12, Px{255, 0, 0, 255});
    PixelBuffer still(8, 6);  // left half green, right half blue
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x) set(still, x, y, x < 4 ? Px{0, 255, 0, 255} : Px{0, 0, 255, 255});
    FrameInputs in = sceneOnly(&scene);
    in.mediaCount = 1;
    in.media[0].frame = &still;
    in.media[0].crop = RectF{0.f, 0.f, 0.5f, 1.f};     // left half only
    in.media[0].fit = FitMode::Stretch;
    in.media[0].region = RectF{0.5f, 0.f, 0.5f, 1.f};  // right half of the canvas
    ComposedFrame f = c.compose(in);
    REQUIRE(f.ok());
    CHECK(at(*f.pixels, 2, 5) == Px{255, 0, 0, 255});  // scene visible left
    CHECK(at(*f.pixels, 8, 0) == Px{0, 255, 0, 255});
    CHECK(at(*f.pixels, 15, 11) == Px{0, 255, 0, 255});
    c.release(f);
    in.media[0].opacity = 0.5f;                         // 128/256
    f = c.compose(in);
    CHECK(at(*f.pixels, 12, 5) == Px{127, 128, 0, 255});
    c.release(f);
    CHECK(sanitizeCrop(RectF{-1, -1, 9, 9}) .w <= 1.f);
    CHECK(sanitizeCrop(RectF{0.9f, 0.9f, 0.5f, 0.5f}).w == doctest::Approx(0.1f));
    CHECK(sanitizeCrop(RectF{0.f, 0.f, 0.f, 0.f}).w == doctest::Approx(1.f));
}
