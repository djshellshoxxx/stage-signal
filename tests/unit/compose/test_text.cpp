#include "util.hpp"
using namespace tu;

namespace {
TextItem item(std::uint32_t id, std::shared_ptr<const PixelBuffer> bm) {
    TextItem t;
    t.id = id;
    t.utf8 = "hello";
    t.bitmap = std::move(bm);
    return t;
}
std::shared_ptr<const PixelBuffer> mask(std::uint32_t w, std::uint32_t h) { return sharedSolid(w, h, Px{255, 255, 255, 255}); }
std::string repeat(const std::string& s, int n) {
    std::string r;
    for (int i = 0; i < n; ++i) r += s;
    return r;
}
}  // namespace

TEST_CASE("REQ-S06: text limits follow section 13 (4096 code points, UTF-8 validity)") {
    TextLayer tl;
    TextItem t = item(1, mask(2, 2));
    t.utf8 = repeat("a", 4096);
    CHECK(tl.publish(t, 0).ok());
    t.utf8 = repeat("a", 4097);
    auto r = tl.publish(t, 0);
    REQUIRE(!r.ok());
    CHECK(r.error().category == ss::ErrorCategory::InvalidArgument);
    t.utf8 = repeat("\xC3\xA9", 4096);                  // 4096 code points, 8192 bytes: accepted
    CHECK(tl.publish(t, 0).ok());
    t.utf8 = repeat("\xF0\x9F\x98\x80", 4097);          // emoji, 4097 code points
    CHECK(!tl.publish(t, 0).ok());
    t.utf8 = "bad \xC0\xAF";                            // overlong
    CHECK(!tl.publish(t, 0).ok());
    t.utf8 = "ok";
    t.bitmap = nullptr;
    CHECK(!tl.publish(t, 0).ok());
    t.bitmap = std::make_shared<PixelBuffer>();
    CHECK(!tl.publish(t, 0).ok());
    t.bitmap = std::make_shared<PixelBuffer>(5000, 2);  // beyond 4096 edge: rejected before any use
    CHECK(!tl.publish(t, 0).ok());
    t.bitmap = mask(2, 2);
    t.durationSec = -1;
    CHECK(!tl.publish(t, 0).ok());
    t.durationSec = 5000;
    CHECK(!tl.publish(t, 0).ok());
    t.durationSec = 0.01;
    CHECK(!tl.publish(t, 0).ok());
}

TEST_CASE("REQ-S06: bounded number of simultaneous items, same id replaces") {
    TextLayer tl;
    for (std::uint32_t i = 1; i <= 4; ++i) CHECK(tl.publish(item(i, mask(2, 2)), 0).ok());
    auto r = tl.publish(item(5, mask(2, 2)), 0);
    REQUIRE(!r.ok());
    CHECK(r.error().category == ss::ErrorCategory::ResourceExhausted);
    CHECK(tl.publish(item(3, mask(2, 2)), 0).ok());     // replace
    CHECK(tl.activeCount(0) == 4);
    tl.clearId(2);
    CHECK(tl.activeCount(0) == 3);
    CHECK(tl.publish(item(5, mask(2, 2)), 0).ok());
    tl.clear();
    CHECK(tl.activeCount(0) == 0);
    // expired slots are reusable
    TextItem t = item(1, mask(2, 2));
    t.durationSec = 1;
    for (std::uint32_t i = 1; i <= 4; ++i) { t.id = i; CHECK(tl.publish(t, 0).ok()); }
    CHECK(tl.publish(item(9, mask(2, 2)), secToNs(2)).ok());
}

TEST_CASE("REQ-S06: fade envelope in/out with duration") {
    CHECK(textEnvelope256(0, 4.0, 1.0) == 0);
    CHECK(textEnvelope256(msToNs(500), 4.0, 1.0) == 128);
    CHECK(textEnvelope256(secToNs(1), 4.0, 1.0) == 256);
    CHECK(textEnvelope256(secToNs(2), 4.0, 1.0) == 256);
    CHECK(textEnvelope256(msToNs(3500), 4.0, 1.0) == 128);
    CHECK(textEnvelope256(secToNs(4), 4.0, 1.0) == 0);
    CHECK(textEnvelope256(secToNs(5), 4.0, 1.0) == 0);
    CHECK(textEnvelope256(msToNs(1000), 4.0, 0.0) == 256);     // no fade
    CHECK(textEnvelope256(msToNs(1), 4.0, 0.0) == 256);
    CHECK(textEnvelope256(secToNs(1), 1.0, 5.0) == 0);          // fade clamped to duration/2
    CHECK(textEnvelope256(msToNs(250), 1.0, 5.0) == 128);       // 0.25 s into a 0.5 s fade-in
    CHECK(textEnvelope256(secToNs(100), 0.0, 1.0) == 256);      // duration 0 = hold
}

TEST_CASE("REQ-S06: placement, size and colour are exact; outside the box is untouched") {
    PixelBuffer canvas = solid(16, 12, Px{0, 0, 0, 255});
    TextLayer tl;
    TextItem t = item(1, mask(2, 2));
    t.x = 0.5f; t.y = 0.5f; t.size = 0.5f;     // 6 px high, 6 px wide
    t.color = Rgb{255, 0, 0};
    t.fadeSec = 0;
    REQUIRE(tl.publish(t, 0).ok());
    tl.draw(canvas, msToNs(100));
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            const bool in = x >= 5 && x < 11 && y >= 3 && y < 9;
            CHECK(at(canvas, x, y) == (in ? Px{255, 0, 0, 255} : Px{0, 0, 0, 255}));
        }
    // top-left anchored (centre at 0,0) is kept on screen
    PixelBuffer c2 = solid(16, 12, Px{0, 0, 0, 255});
    TextLayer t2;
    t = item(2, mask(2, 2));
    t.x = 0; t.y = 0; t.size = 0.5f; t.fadeSec = 0; t.color = Rgb{0, 255, 0};
    REQUIRE(t2.publish(t, 0).ok());
    t2.draw(c2, 0);
    CHECK(at(c2, 0, 0) == Px{0, 255, 0, 255});
    CHECK(at(c2, 5, 5) == Px{0, 255, 0, 255});
    CHECK(at(c2, 6, 6) == Px{0, 0, 0, 255});
}

TEST_CASE("REQ-S06: fade scales the premultiplied colour; pre-coloured bitmaps ignore the tint") {
    PixelBuffer canvas = solid(16, 12, Px{0, 0, 0, 255});
    TextLayer tl;
    TextItem t = item(1, mask(2, 2));
    t.size = 0.5f; t.fadeSec = 1.0; t.durationSec = 4.0; t.color = Rgb{200, 200, 200};
    REQUIRE(tl.publish(t, 0).ok());
    tl.draw(canvas, msToNs(500));   // envelope 128/256
    CHECK(at(canvas, 8, 6) == Px{100, 100, 100, 255});

    PixelBuffer c2 = solid(16, 12, Px{0, 0, 0, 255});
    TextLayer t2;
    TextItem col = item(1, sharedSolid(2, 2, Px{0, 0, 255, 255}));
    col.colorBitmap = true; col.color = Rgb{255, 0, 0}; col.size = 0.5f; col.fadeSec = 0;
    REQUIRE(t2.publish(col, 0).ok());
    t2.draw(c2, 0);
    CHECK(at(c2, 8, 6) == Px{0, 0, 255, 255});
}

TEST_CASE("REQ-S06: bounded scroll is a single right-to-left pass, never longer than 120 s") {
    TextLayer tl;
    TextItem t = item(1, mask(4, 2));
    t.scroll = true; t.size = 0.5f; t.durationSec = 4.0; t.color = Rgb{255, 255, 255};
    REQUIRE(tl.publish(t, 0).ok());
    PixelBuffer c0 = solid(16, 12, Px{0, 0, 0, 255});
    tl.draw(c0, 0);
    CHECK(allEqual(c0, Px{0, 0, 0, 255}));              // starts fully off the right edge
    PixelBuffer c1 = solid(16, 12, Px{0, 0, 0, 255});
    tl.draw(c1, secToNs(2));                            // half way: bitmap is 12x6 (size .5 of 12, aspect 2:1)
    int lit = 0;
    for (int x = 0; x < 16; ++x) lit += at(c1, x, 6) != Px{0, 0, 0, 255};
    CHECK(lit > 0);
    CHECK(lit < 16);
    PixelBuffer c2 = solid(16, 12, Px{0, 0, 0, 255});
    tl.draw(c2, secToNs(4));                            // finished: off the left edge, expired
    CHECK(allEqual(c2, Px{0, 0, 0, 255}));
    CHECK(tl.activeCount(secToNs(4)) == 0);
    CHECK(tl.activeCount(secToNs(3)) == 1);
    // duration clamps: < 2 s => 2 s ; > 120 s => 120 s ; 0 => default 8 s
    CHECK(scrollProgress(secToNs(1), 0.5) == doctest::Approx(0.5));
    CHECK(scrollProgress(secToNs(60), 9999.0) == doctest::Approx(0.5));
    CHECK(scrollProgress(secToNs(4), 0.0) == doctest::Approx(0.5));
    CHECK(scrollProgress(-5, 4.0) == 0.0);
    tl.purgeExpired(secToNs(10));
    CHECK(tl.publish(item(2, mask(2, 2)), secToNs(10)).ok());
}

TEST_CASE("REQ-S06: over-wide static text scales down to fit the canvas instead of clipping") {
    PixelBuffer canvas = solid(16, 12, Px{0, 0, 0, 255});
    TextLayer tl;
    TextItem t = item(1, mask(64, 4));
    t.size = 0.5f; t.fadeSec = 0;
    REQUIRE(tl.publish(t, 0).ok());
    tl.draw(canvas, 0);
    // fitted: 16 x 1, vertically centred (cy=6 -> top 6 - 0)
    int rows = 0;
    for (int y = 0; y < 12; ++y) {
        if (at(canvas, 0, y) != Px{0, 0, 0, 255}) {
            ++rows;
            CHECK(at(canvas, 15, y) == at(canvas, 0, y));
        }
    }
    CHECK(rows == 1);
}

TEST_CASE("REQ-S06: policy showText=false hides published text") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12).ok());
    const PixelBuffer scene = solid(16, 12, Px{0, 0, 0, 255});
    TextLayer tl;
    TextItem t = item(1, mask(2, 2));
    t.size = 1.f; t.fadeSec = 0; t.color = Rgb{255, 255, 255};
    REQUIRE(tl.publish(t, 0).ok());
    FrameInputs in = sceneOnly(&scene);
    in.text = &tl;
    ComposedFrame f = comp.compose(in);
    CHECK(at(*f.pixels, 8, 6) == Px{255, 255, 255, 255});
    comp.release(f);
    CompositorConfig cfg;
    cfg.policy.showText = false;
    comp.setConfig(cfg);
    f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 0, 0, 255}));
    comp.release(f);
}
