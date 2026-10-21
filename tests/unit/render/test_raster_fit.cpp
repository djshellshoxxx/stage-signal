#include <cmath>
#include "ss/render/fit.hpp"
#include "ss/render/fixed.hpp"
#include "ss/render/raster.hpp"
#include "test_util.hpp"

using namespace ss;
using namespace ss::render;
using fx::Rgb8;

namespace {
PixelBuffer black(std::uint32_t w, std::uint32_t h) {
    PixelBuffer b(w, h);
    for (std::size_t i = 3; i < b.rgba.size(); i += 4) b.rgba[i] = 255;
    return b;
}
int px(const PixelBuffer& b, int x, int y, int c = 0) { return b.row(std::uint32_t(y))[x * 4 + c]; }
}  // namespace

TEST_CASE("fixed: sine, atan2, sqrt, phase, hsv helpers") {
    for (int i = 0; i < 4096; ++i) {
        const double a = i / 4096.0;
        CHECK(std::fabs(fx::sinQ15(std::uint32_t(i * 16)) / 32768.0 - std::sin(a * 6.283185307179586)) < 0.012);
    }
    for (int deg = 0; deg < 360; ++deg) {
        const double r = deg * 3.141592653589793 / 180.0;
        const std::uint32_t a = fx::atan2Q16(std::int32_t(std::lround(std::sin(r) * 1000)), std::int32_t(std::lround(std::cos(r) * 1000)));
        double e = std::fabs(a / 65536.0 * 360.0 - deg);
        if (e > 180) e = 360 - e;
        CHECK(e < 0.6);
    }
    for (std::uint64_t v : {0ull, 1ull, 2ull, 99ull, 100ull, 65535ull, 1000000007ull, 0xFFFFFFFFFFFFFFFFull})
        CHECK(std::uint64_t(fx::isqrt64(v)) * fx::isqrt64(v) <= v);
    CHECK(fx::isqrt64(1000000ull) == 1000);
    // phase comes from elapsed time only and wraps; speed 2 is twice as fast
    CHECK(fx::phaseQ16(0, 256, 1000) == 0);
    CHECK(fx::phaseQ16(500'000'000, 256, 1000) == 32768);
    CHECK(fx::phaseQ16(1'500'000'000, 256, 1000) == 32768);
    CHECK(fx::phaseQ16(250'000'000, 512, 1000) == 32768);
    CHECK(fx::phaseQ16(-5, 256, 1000) == 0);
    CHECK(fx::phaseQ16(3'000'000'000'000'000LL, 2048, 7) < 65536u);   // huge times do not overflow
    const Rgb8 red = fx::hsv(0, 256, 255), grey = fx::hsv(12345, 0, 100), green = fx::hsv(65536 / 3, 256, 255);
    CHECK((red.r == 255 && red.g == 0 && red.b == 0));
    CHECK((grey.r == 100 && grey.g == 100 && grey.b == 100));
    CHECK((green.g == 255 && green.r < 3 && green.b < 3));
    CHECK(fx::div255(255 * 255) == 255);
    CHECK(fx::div255(0) == 0);
    CHECK(fx::toQ8(0.5f) == 128);
    CHECK(fx::toQ8(2.f) == 256);
    CHECK(fx::toQ8(std::nanf("")) == 0);
}

TEST_CASE("raster: fill, rect clipping, gradients, blending") {
    PixelBuffer b(32, 24);
    b.rgba.resize(b.rgba.size() + 128, 0xAB);
    Canvas c(b);
    REQUIRE(c.valid());
    fill(c, {10, 20, 30});
    CHECK(px(b, 31, 23, 2) == 30);
    CHECK(px(b, 0, 0, 3) == 255);
    fillRect(c, -5, -5, 12, 12, {200, 0, 0});          // clipped on two sides
    CHECK(px(b, 0, 0) == 200);
    CHECK(px(b, 6, 6) == 200);
    CHECK(px(b, 7, 7) == 10);
    fillRect(c, 30, 22, 100, 100, {0, 200, 0});        // clipped on the far side
    CHECK(px(b, 31, 23, 1) == 200);
    fillRect(c, 1000, 1000, 5, 5, {1, 1, 1});          // fully outside, no-ops
    fillRect(c, 0, 0, -3, 5, {1, 1, 1});
    fillRect(c, 0, 0, 2'000'000'000, 2'000'000'000, {9, 9, 9}, Blend::Add);   // no int overflow
    CHECK(px(b, 31, 23, 1) == 209);
    fill(c, {0, 0, 0});
    gradientV(c, 0, 0, 32, 24, {0, 0, 0}, {240, 240, 240});
    CHECK(px(b, 5, 0) == 0);
    CHECK(px(b, 5, 23) == 240);
    CHECK(px(b, 5, 12) > 100);
    gradientH(c, 0, 0, 32, 24, {255, 0, 0}, {0, 0, 255});
    CHECK(px(b, 0, 3) == 255);
    CHECK(px(b, 31, 3, 2) == 255);
    fill(c, {250, 10, 10});
    putAdd(c, 2, 2, {50, 50, 50});
    CHECK(px(b, 2, 2) == 255);                         // saturating additive
    CHECK(px(b, 2, 2, 1) == 60);
    putCover(c, 4, 4, {0, 0, 0}, 128, Blend::Over);
    CHECK(px(b, 4, 4) > 120);
    CHECK(px(b, 4, 4) < 130);
    putCover(c, -1, 4, {0, 0, 0}, 128, Blend::Over);   // out of range ignored
    for (std::size_t i = 32 * 24 * 4; i < b.rgba.size(); ++i) REQUIRE(b.rgba[i] == 0xAB);
    PixelBuffer shortBuf(32, 24);
    shortBuf.rgba.resize(10);
    CHECK_FALSE(Canvas(shortBuf).valid());
}

TEST_CASE("raster: anti-aliased lines, circles, rings, glow") {
    PixelBuffer b = black(64, 48);
    b.rgba.resize(b.rgba.size() + 128, 0xAB);
    Canvas c(b);
    lineAA(c, 4 * 256, 10 * 256, 59 * 256, 10 * 256, {255, 255, 255}, Blend::Add);   // horizontal on pixel centres
    CHECK(px(b, 30, 10) == 255);
    CHECK(px(b, 30, 11) == 0);
    CHECK(px(b, 30, 9) == 0);
    lineAA(c, 4 * 256, 20 * 256, 59 * 256, 40 * 256, {255, 255, 255}, Blend::Add);   // diagonal covers two rows per column
    int lit = 0;
    for (int x = 4; x <= 59; ++x) lit += px(b, x, 20 + (x - 4) * 20 / 55) > 100 || px(b, x, 21 + (x - 4) * 20 / 55) > 100;
    CHECK(lit >= 54);
    lineAA(c, 10 * 256, 2 * 256, 10 * 256, 45 * 256, {0, 255, 0}, Blend::Add);       // vertical
    CHECK(px(b, 10, 30, 1) == 255);
    lineAA(c, -100000, -100000, 100000, 100000, {9, 9, 9}, Blend::Add, 3);           // wildly out of range: clipped, no crash
    lineAA(c, 0, 0, 0, 0, {9, 9, 9});                                                // degenerate
    const long long before = [&] { long long s = 0; for (int i = 0; i < 64 * 48 * 4; ++i) s += b.rgba[std::size_t(i)]; return s; }();
    CHECK(before > 0);
    PixelBuffer d = black(64, 64);
    Canvas cd(d);
    fillCircleAA(cd, 32 * 256, 32 * 256, 20 * 256, {255, 255, 255}, Blend::Over);
    double area = 0;
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) area += px(d, x, y) / 255.0;
    CHECK(std::fabs(area - 3.14159265 * 400.0) / (3.14159265 * 400.0) < 0.03);
    CHECK(px(d, 32, 32) == 255);
    CHECK(px(d, 2, 2) == 0);
    PixelBuffer r = black(64, 64);
    ringAA(Canvas(r), 32 * 256, 32 * 256, 20 * 256, 3 * 256, {255, 255, 255}, Blend::Add);
    CHECK(px(r, 52, 32) > 200);
    CHECK(px(r, 32, 32) == 0);
    CHECK(px(r, 42, 32) == 0);
    PixelBuffer g = black(64, 64);
    glowDisc(Canvas(g), 32 * 256, 32 * 256, 16 * 256, {255, 255, 255}, 256);
    CHECK(px(g, 32, 32) > px(g, 40, 32));
    CHECK(px(g, 40, 32) > px(g, 46, 32));
    CHECK(px(g, 50, 32) == 0);
    fillCircleAA(cd, 1000 * 256, 1000 * 256, 5 * 256, {1, 1, 1});   // outside: nothing
    fillCircleAA(cd, 0, 0, 0, {1, 1, 1});
    for (std::size_t i = 64 * 48 * 4; i < b.rgba.size(); ++i) REQUIRE(b.rgba[i] == 0xAB);
}

TEST_CASE("REQ-D04: aspect fit into an 800x600 target") {
    CHECK(fitRect(400, 300, 800, 600, FitMode::Contain) == RectI{0, 0, 800, 600});          // 4:3 fills exactly
    CHECK(fitRect(1920, 1080, 800, 600, FitMode::Contain) == RectI{0, 75, 800, 450});       // 16:9 letterboxed
    CHECK(fitRect(1080, 1920, 800, 600, FitMode::Contain) == RectI{231, 0, 338, 600});      // portrait pillarboxed
    CHECK(fitRect(400, 300, 800, 600, FitMode::Stretch) == RectI{0, 0, 800, 600});
    const RectI cover = fitRect(1920, 1080, 800, 600, FitMode::Cover);
    CHECK(cover.h == 600);
    CHECK(cover.w >= 800);
    CHECK(cover.x == (800 - cover.w) / 2);
    CHECK(fitRect(0, 10, 800, 600, FitMode::Contain) == RectI{});
    CHECK(fitRect(10, 10, 0, 600, FitMode::Contain) == RectI{});
    for (int w : {1, 3, 160, 400, 641, 1920})
        for (int h : {1, 3, 120, 300, 480, 1080}) {
            const RectI r = fitRect(w, h, 800, 600, FitMode::Contain);
            CHECK(r.w >= 1);
            CHECK(r.h >= 1);
            CHECK(r.w <= 800);
            CHECK(r.h <= 600);
            CHECK(r.x >= 0);
            CHECK(r.y >= 0);
            CHECK(r.x + r.w <= 800);
            CHECK(r.y + r.h <= 600);
        }
}

TEST_CASE("REQ-D04: reference borders stay visible after the fit blit") {
    for (auto sz : {std::pair<int, int>{400, 300}, {640, 360}, {270, 480}, {1000, 1000}}) {
        INFO(sz.first << "x" << sz.second);
        PixelBuffer src = black(std::uint32_t(sz.first), std::uint32_t(sz.second));
        for (int x = 0; x < sz.first; ++x) for (int y = 0; y < sz.second; ++y)
            if (x == 0 || y == 0 || x == sz.first - 1 || y == sz.second - 1) { std::uint8_t* p = src.row(std::uint32_t(y)) + x * 4; p[0] = 255; p[1] = 255; p[2] = 255; }
        PixelBuffer dst(800, 600);
        REQUIRE(blitFit(src, dst, FitMode::Contain, 0));
        const RectI r = fitRect(sz.first, sz.second, 800, 600, FitMode::Contain);
        for (int x = r.x; x < r.x + r.w; ++x) { CHECK(px(dst, x, r.y) == 255); CHECK(px(dst, x, r.y + r.h - 1) == 255); }
        for (int y = r.y; y < r.y + r.h; ++y) { CHECK(px(dst, r.x, y) == 255); CHECK(px(dst, r.x + r.w - 1, y) == 255); }
        if (r.x > 0) CHECK(px(dst, 0, 300) == 0);          // letterbox is opaque black
        if (r.y > 0) CHECK(px(dst, 400, 0) == 0);
        CHECK(px(dst, 0, 0, 3) == 255);
    }
}

TEST_CASE("REQ-D07: rotation and resolution changes recompute composition, content is preserved") {
    int w, h;
    rotatedSize(800, 600, 90, w, h);  CHECK((w == 600 && h == 800));
    rotatedSize(800, 600, 270, w, h); CHECK((w == 600 && h == 800));
    rotatedSize(800, 600, 180, w, h); CHECK((w == 800 && h == 600));
    rotatedSize(800, 600, -90, w, h); CHECK((w == 600 && h == 800));   // -90 == 270
    rotatedSize(800, 600, 45, w, h);  CHECK((w == 800 && h == 600));   // unsupported angles are 0
    // 4:3 content rotated 90 degrees becomes 3:4 and is pillarboxed in a 4:3 output
    CHECK(fitRect(600, 800, 800, 600, FitMode::Contain) == RectI{175, 0, 450, 600});
    // marker pixel travels with the content: red at the top-left of a 4x2 source
    PixelBuffer src = black(4, 2);
    src.row(0)[0] = 255;
    struct Case { int rot, ex, ey; };
    for (auto cs : {Case{0, 0, 0}, Case{90, 1, 0}, Case{180, 3, 1}, Case{270, 0, 3}}) {
        INFO("rot " << cs.rot);
        PixelBuffer dst(cs.rot == 90 || cs.rot == 270 ? 2u : 4u, cs.rot == 90 || cs.rot == 270 ? 4u : 2u);
        REQUIRE(blitFit(src, dst, FitMode::Contain, cs.rot));
        CHECK(px(dst, cs.ex, cs.ey) == 255);
        int reds = 0;
        for (std::uint32_t y = 0; y < dst.height; ++y) for (std::uint32_t x = 0; x < dst.width; ++x) reds += px(dst, int(x), int(y)) == 255;
        CHECK(reds == 1);
    }
    // aspect checks for 4:3, 16:9 and portrait outputs with the same show content
    for (auto out : {std::pair<int, int>{800, 600}, {1280, 720}, {720, 1280}}) {
        const RectI r = fitRect(400, 300, out.first, out.second, FitMode::Contain);
        CHECK(std::abs(double(r.w) / r.h - 4.0 / 3.0) < 0.02);
        CHECK(r.x == (out.first - r.w) / 2);
    }
    const RectI n = normToPixels(0.25f, 0.5f, 0.5f, 0.5f, 800, 600);
    CHECK(n == RectI{200, 300, 400, 300});
    const RectI a = normToPixels(0.f, 0.f, 0.3333f, 1.f, 100, 100), bb = normToPixels(0.3333f, 0.f, 0.6667f, 1.f, 100, 100);
    CHECK(a.x + a.w == bb.x);                                       // adjacent rectangles tile without gaps
    CHECK(bb.x + bb.w == 100);
    CHECK(normToPixels(-1.f, 2.f, 5.f, 5.f, 10, 10).w == 10);
    PixelBuffer empty, d(10, 10);
    CHECK_FALSE(blitFit(empty, d, FitMode::Contain, 0));
    CHECK_FALSE(blitFit(d, empty, FitMode::Contain, 0));
    PixelBuffer s2 = black(400, 300), d2(800, 600);
    std::size_t allocs;
    {
        alloc_probe::Scope scope;
        blitFit(s2, d2, FitMode::Contain, 90);
        allocs = scope.n();
    }
    CHECK(allocs == 0);
}
