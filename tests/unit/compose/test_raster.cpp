#include "util.hpp"
#include <cmath>
using namespace tu;

TEST_CASE("premultiplied math: div255 is exact rounding and never exceeds 255") {
    for (std::uint32_t x = 0; x <= 255u * 255u; ++x) {
        const std::uint32_t expect = std::uint32_t(std::floor(double(x) / 255.0 + 0.5));
        REQUIRE(div255(x) == expect);
        REQUIRE(div255(x) <= 255u);
    }
}

TEST_CASE("premultiplied math: over() matches the real-number reference within 1 and cannot overflow") {
    for (int sa = 0; sa <= 255; sa += 5)
        for (int sc = 0; sc <= sa; sc += 5)
            for (int da = 0; da <= 255; da += 5)
                for (int dc = 0; dc <= da; dc += 5) {
                    const Px r = over(Px{std::uint8_t(sc), 0, 0, std::uint8_t(sa)}, Px{std::uint8_t(dc), 0, 0, std::uint8_t(da)});
                    const double ref = sc + dc * (255 - sa) / 255.0;
                    REQUIRE(std::fabs(double(r.r) - ref) <= 1.0);
                    REQUIRE(r.r <= r.a);  // premultiplied invariant survives
                }
    // opaque source replaces, transparent source keeps destination
    CHECK(over(Px{9, 8, 7, 255}, Px{1, 2, 3, 255}) == Px{9, 8, 7, 255});
    CHECK(over(Px{0, 0, 0, 0}, Px{1, 2, 3, 255}) == Px{1, 2, 3, 255});
}

TEST_CASE("premultiplied math: invalid input (colour > alpha) saturates instead of wrapping") {
    const Px r = over(Px{200, 200, 200, 100}, Px{200, 200, 200, 255});
    CHECK(r.r == 255);
    CHECK(r.g == 255);
    CHECK(r.a == 255);
    CHECK(scale256(Px{255, 255, 255, 255}, 256) == Px{255, 255, 255, 255});
    CHECK(scale256(Px{255, 255, 255, 255}, 0) == Px{0, 0, 0, 0});
}

TEST_CASE("REQ-D04: aspect fit geometry for 4:3, 16:9 and portrait targets") {
    // 800x600 source (4:3)
    auto f = computeFit(800, 600, Rect{0, 0, 800, 600}, FitMode::Fit);
    CHECK(f.dst == Rect{0, 0, 800, 600});
    f = computeFit(800, 600, Rect{0, 0, 1280, 720}, FitMode::Fit);  // 16:9: pillarbox
    CHECK(f.dst == Rect{160, 0, 960, 720});
    f = computeFit(800, 600, Rect{0, 0, 600, 800}, FitMode::Fit);   // portrait: letterbox
    CHECK(f.dst == Rect{0, 175, 600, 450});
    f = computeFit(1920, 1080, Rect{0, 0, 800, 600}, FitMode::Fit); // wide source into 4:3
    CHECK(f.dst == Rect{0, 75, 800, 450});
    // crop: region filled, source window centred
    f = computeFit(800, 600, Rect{0, 0, 1280, 720}, FitMode::Crop);
    CHECK(f.dst == Rect{0, 0, 1280, 720});
    CHECK(f.src == Rect{0, 75, 800, 450});
    // stretch
    f = computeFit(800, 600, Rect{0, 0, 600, 800}, FitMode::Stretch);
    CHECK(f.dst == Rect{0, 0, 600, 800});
    CHECK(f.src == Rect{0, 0, 800, 600});
}

namespace {
PixelBuffer borderReference(std::uint32_t w, std::uint32_t h, Px border, Px inner) {
    PixelBuffer b = solid(w, h, inner);
    for (std::uint32_t x = 0; x < w; ++x) { set(b, int(x), 0, border); set(b, int(x), int(h) - 1, border); }
    for (std::uint32_t y = 0; y < h; ++y) { set(b, 0, int(y), border); set(b, int(w) - 1, int(y), border); }
    return b;
}
bool ringIs(const PixelBuffer& b, Rect r, Px c) {
    for (int x = r.x; x < r.x + r.w; ++x)
        if (at(b, x, r.y) != c || at(b, x, r.y + r.h - 1) != c) return false;
    for (int y = r.y; y < r.y + r.h; ++y)
        if (at(b, r.x, y) != c || at(b, r.x + r.w - 1, y) != c) return false;
    return true;
}
}  // namespace

TEST_CASE("REQ-D04: 1 px reference border stays visible at 800x600 and other aspect ratios (both samplers)") {
    const Px red{255, 0, 0, 255}, blue{0, 0, 255, 255};
    const PixelBuffer ref = borderReference(800, 600, red, blue);
    struct Target { std::uint32_t w, h; };
    const Target targets[] = {{800, 600}, {1024, 768}, {1280, 720}, {600, 800}, {640, 480}, {400, 300}};
    for (Sampling sm : {Sampling::Nearest, Sampling::Bilinear}) {
        for (const Target& t : targets) {
            Compositor c;
            REQUIRE(c.prepare(t.w, t.h, 1.0f).ok());
            CompositorConfig cfg;
            cfg.sceneSampling = sm;
            cfg.background = Rgb{0, 0, 0};
            c.setConfig(cfg);
            ComposedFrame f = c.compose(sceneOnly(&ref));
            REQUIRE(f.ok());
            const FitResult fr = computeFit(800, 600, Rect{0, 0, int(t.w), int(t.h)}, FitMode::Fit);
            CAPTURE(t.w);
            CAPTURE(t.h);
            CHECK(ringIs(*f.pixels, fr.dst, red));
            // centre of the picture shows the content, bars show the background
            CHECK(at(*f.pixels, fr.dst.x + fr.dst.w / 2, fr.dst.y + fr.dst.h / 2) == blue);
            if (fr.dst.x > 0) CHECK(at(*f.pixels, 0, int(t.h) / 2) == Px{0, 0, 0, 255});
            c.release(f);
        }
    }
}

TEST_CASE("REQ-D04: portrait via rotation keeps the border visible on the rotated composition") {
    const Px red{255, 0, 0, 255}, blue{0, 0, 255, 255};
    const PixelBuffer ref = borderReference(800, 600, red, blue);
    for (int rot : {0, 90, 180, 270}) {
        Compositor c;
        const bool swap = rot == 90 || rot == 270;
        REQUIRE(c.prepare(swap ? 600 : 800, swap ? 800 : 600, 1.0f, 3, rot).ok());
        ComposedFrame f = c.compose(sceneOnly(&ref));
        REQUIRE(f.ok());
        CHECK(ringIs(*f.pixels, Rect{0, 0, int(f.pixels->width), int(f.pixels->height)}, red));
        c.release(f);
    }
}

TEST_CASE("corner aligned sampling: first and last destination pixels map to first and last source pixels") {
    const PixelBuffer src = coords(5, 4);
    PixelBuffer dst(23, 17);
    for (Sampling sm : {Sampling::Nearest, Sampling::Bilinear}) {
        DrawParams p;
        p.sampling = sm;
        p.blendOver = false;
        drawScaled(src, Rect{0, 0, 5, 4}, dst, Rect{0, 0, 23, 17}, p);
        CHECK(at(dst, 0, 0) == at(src, 0, 0));
        CHECK(at(dst, 22, 0) == at(src, 4, 0));
        CHECK(at(dst, 0, 16) == at(src, 0, 3));
        CHECK(at(dst, 22, 16) == at(src, 4, 3));
    }
}

TEST_CASE("nearest and bilinear identity copies are exact") {
    const PixelBuffer src = coords(16, 12);
    for (Sampling sm : {Sampling::Nearest, Sampling::Bilinear}) {
        PixelBuffer dst(16, 12);
        DrawParams p;
        p.sampling = sm;
        p.blendOver = false;
        drawScaled(src, Rect{0, 0, 16, 12}, dst, Rect{0, 0, 16, 12}, p);
        CHECK(dst.rgba == src.rgba);
    }
}

TEST_CASE("bilinear upscale of a two-pixel ramp is monotonic and bounded by the endpoints") {
    PixelBuffer src(2, 1);
    set(src, 0, 0, Px{0, 0, 0, 255});
    set(src, 1, 0, Px{200, 100, 50, 255});
    PixelBuffer dst(9, 1);
    DrawParams p;
    p.blendOver = false;
    drawScaled(src, Rect{0, 0, 2, 1}, dst, Rect{0, 0, 9, 1}, p);
    int prev = -1;
    for (int x = 0; x < 9; ++x) {
        const Px v = at(dst, x, 0);
        CHECK(int(v.r) >= prev);
        CHECK(v.r <= 200);
        CHECK(v.a == 255);
        prev = v.r;
    }
    CHECK(at(dst, 8, 0) == Px{200, 100, 50, 255});
}

TEST_CASE("rotateInto: clockwise 90/180/270 golden on a 3x2 image") {
    PixelBuffer s(3, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) set(s, x, y, Px{std::uint8_t(y * 3 + x + 1), 0, 0, 255});
    // s = [1 2 3 / 4 5 6]
    PixelBuffer r90(2, 3), r180(3, 2), r270(2, 3);
    rotateInto(s, 90, r90);
    rotateInto(s, 180, r180);
    rotateInto(s, 270, r270);
    const int e90[3][2] = {{4, 1}, {5, 2}, {6, 3}};
    const int e180[2][3] = {{6, 5, 4}, {3, 2, 1}};
    const int e270[3][2] = {{3, 6}, {2, 5}, {1, 4}};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 2; ++x) {
            CHECK(at(r90, x, y).r == e90[y][x]);
            CHECK(at(r270, x, y).r == e270[y][x]);
        }
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) CHECK(at(r180, x, y).r == e180[y][x]);
}

TEST_CASE("toPixelRect clamps, rejects NaN and never returns an empty rectangle") {
    CHECK(toPixelRect(RectF{0, 0, 1, 1}, 16, 12) == Rect{0, 0, 16, 12});
    CHECK(toPixelRect(RectF{0.5f, 0.5f, 0.5f, 0.5f}, 16, 12) == Rect{8, 6, 8, 6});
    CHECK(toPixelRect(RectF{-3, -3, 99, 99}, 16, 12) == Rect{0, 0, 16, 12});
    const Rect z = toPixelRect(RectF{0.5f, 0.5f, 0.f, 0.f}, 16, 12);
    CHECK(z.w >= 1);
    CHECK(z.h >= 1);
    const Rect n = toPixelRect(RectF{std::nanf(""), std::nanf(""), 1.f, 1.f}, 16, 12);
    CHECK(!n.empty());
}
