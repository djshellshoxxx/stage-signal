#include "util.hpp"
using namespace tu;

namespace {
CameraLayer makeCam(const CameraLayerConfig& cfg, std::uint32_t w = 16, std::uint32_t h = 12) {
    CameraLayer c;
    REQUIRE(c.prepare(w, h).ok());
    c.configure(cfg);
    return c;
}
}  // namespace

TEST_CASE("REQ-S10: mirror flips horizontally") {
    PixelBuffer b(4, 1);
    for (int x = 0; x < 4; ++x) set(b, x, 0, Px{std::uint8_t(x + 1), 0, 0, 255});
    mirrorHorizontal(b);
    CHECK(at(b, 0, 0).r == 4);
    CHECK(at(b, 1, 0).r == 3);
    CHECK(at(b, 2, 0).r == 2);
    CHECK(at(b, 3, 0).r == 1);
    PixelBuffer odd(3, 1);
    for (int x = 0; x < 3; ++x) set(odd, x, 0, Px{std::uint8_t(x + 1), 0, 0, 255});
    mirrorHorizontal(odd);
    CHECK(at(odd, 0, 0).r == 3);
    CHECK(at(odd, 1, 0).r == 2);
}

TEST_CASE("REQ-S10: tint blends the configured colour 0..1") {
    const Px src{100, 150, 200, 255};
    PixelBuffer b = solid(2, 2, src);
    tint(b, Rgb{255, 0, 0}, 0.f);
    CHECK(allEqual(b, src));
    tint(b, Rgb{255, 0, 0}, 1.f);
    CHECK(allEqual(b, Px{255, 0, 0, 255}));
    b = solid(2, 2, src);
    tint(b, Rgb{255, 0, 0}, 0.5f);
    CHECK(allEqual(b, Px{178, 75, 100, 255}));
    b = solid(2, 2, src);
    tint(b, Rgb{255, 0, 0}, 9.f);  // clamped to 1
    CHECK(allEqual(b, Px{255, 0, 0, 255}));
    // translucent premultiplied pixel stays valid
    b = solid(1, 1, Px{50, 50, 50, 100});
    tint(b, Rgb{255, 255, 255}, 1.f);
    CHECK(at(b, 0, 0).a == 100);
    CHECK(at(b, 0, 0).r <= 100);
}

TEST_CASE("REQ-S10: threshold maps luminance around the cutoff") {
    PixelBuffer b(4, 1);
    set(b, 0, 0, Px{200, 200, 200, 255});
    set(b, 1, 0, Px{100, 100, 100, 255});
    set(b, 2, 0, Px{128, 128, 128, 255});   // luma == cutoff(128) -> white
    set(b, 3, 0, Px{255, 0, 0, 255});       // luma 54 -> black
    threshold(b, 0.5f);
    CHECK(at(b, 0, 0) == Px{255, 255, 255, 255});
    CHECK(at(b, 1, 0) == Px{0, 0, 0, 255});
    CHECK(at(b, 2, 0) == Px{255, 255, 255, 255});
    CHECK(at(b, 3, 0) == Px{0, 0, 0, 255});
    PixelBuffer c = solid(1, 1, Px{10, 10, 10, 255});
    threshold(c, 0.f);
    CHECK(at(c, 0, 0) == Px{255, 255, 255, 255});
    c = solid(1, 1, Px{254, 254, 254, 255});
    threshold(c, 1.f);
    CHECK(at(c, 0, 0) == Px{0, 0, 0, 255});
}

TEST_CASE("REQ-S10: posterize 2..16 levels, values land on level centres") {
    PixelBuffer b(4, 1);
    set(b, 0, 0, Px{0, 127, 128, 255});
    set(b, 1, 0, Px{255, 100, 200, 255});
    posterize(b, 2);
    CHECK(at(b, 0, 0) == Px{0, 0, 255, 255});
    CHECK(at(b, 1, 0) == Px{255, 0, 255, 255});
    PixelBuffer c = solid(1, 1, Px{100, 200, 255, 255});
    posterize(c, 4);   // levels 0, 85, 170, 255
    CHECK(at(c, 0, 0) == Px{85, 170, 255, 255});
    // clamping: 1 -> 2 levels, 99 -> 16 levels
    PixelBuffer d = solid(1, 1, Px{100, 100, 100, 255});
    posterize(d, 1);
    CHECK(at(d, 0, 0).r == 0);
    PixelBuffer e = solid(1, 1, Px{17, 34, 51, 255});
    posterize(e, 99);   // 16 levels: step 17, values already on levels
    CHECK(at(e, 0, 0) == Px{17, 34, 51, 255});
    CHECK(sanitize(CameraTreatments{false, true, 1}).posterizeLevels == 2);
    CHECK(sanitize(CameraTreatments{false, true, 99}).posterizeLevels == 16);
}

TEST_CASE("REQ-S10: treatment order is mirror, posterize, threshold, tint") {
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.treatments.mirror = true;
    cfg.treatments.thresholdOn = true;
    cfg.treatments.thresholdCutoff = 0.5f;
    cfg.treatments.tintOn = true;
    cfg.treatments.tintColor = Rgb{255, 0, 0};
    cfg.treatments.tintAmount = 1.f;
    CameraLayer cam = makeCam(cfg, 2, 1);
    PixelBuffer in(2, 1);
    set(in, 0, 0, Px{255, 255, 255, 255});
    set(in, 1, 0, Px{0, 0, 0, 255});
    REQUIRE(cam.pushFrame(1, in, 0));
    // tint after threshold with amount 1 -> everything red regardless of the mirrored threshold result
    CHECK(at(*cam.frame(), 0, 0) == Px{255, 0, 0, 255});
    cfg.treatments.tintAmount = 0.f;
    cam.configure(cfg);
    REQUIRE(cam.pushFrame(1, in, 1));
    CHECK(at(*cam.frame(), 0, 0) == Px{0, 0, 0, 255});        // mirrored: black first
    CHECK(at(*cam.frame(), 1, 0) == Px{255, 255, 255, 255});
}

TEST_CASE("REQ-S10: motion trail uses 2..8 bounded frames and fades by (n-1)/n, floor") {
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.treatments.trailFrames = 4;
    CameraLayer cam = makeCam(cfg, 2, 2);
    const PixelBuffer white = solid(2, 2, Px{255, 255, 255, 255}), black = solid(2, 2, Px{0, 0, 0, 255});
    REQUIRE(cam.pushFrame(1, white, 0));
    CHECK(at(*cam.frame(), 0, 0).r == 255);
    const int expect[] = {191, 143, 107, 80, 60};   // floor(prev * 3 / 4)
    for (int e : expect) {
        REQUIRE(cam.pushFrame(1, black, 0));
        CHECK(at(*cam.frame(), 1, 1).r == e);
        CHECK(at(*cam.frame(), 1, 1).a == 255);
    }
    for (int i = 0; i < 40; ++i) cam.pushFrame(1, black, 0);
    CHECK(at(*cam.frame(), 0, 0).r == 0);                    // always fades to nothing
    // bounds
    CHECK(sanitize(CameraTreatments{false, false, 4, false, 0.5f, false, Rgb{}, 0.5f, 1}).trailFrames == 2);
    CHECK(sanitize(CameraTreatments{false, false, 4, false, 0.5f, false, Rgb{}, 0.5f, 99}).trailFrames == 8);
    CHECK(sanitize(CameraTreatments{}).trailFrames == 0);
}

TEST_CASE("REQ-S10: trail is the first thing shed under pressure (quality step 1) and returns afterwards") {
    Compositor comp;
    REQUIRE(comp.prepare(8, 6).ok());
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.published = true;
    cfg.treatments.trailFrames = 8;
    CameraLayer cam;
    REQUIRE(cam.prepare(8, 6).ok());
    cam.configure(cfg);
    const PixelBuffer scene = solid(8, 6, Px{0, 0, 0, 255});
    const PixelBuffer white = solid(8, 6, Px{255, 255, 255, 255}), black = solid(8, 6, Px{0, 0, 0, 255});
    FrameInputs in = sceneOnly(&scene);
    in.camera = &cam;
    cam.pushFrame(1, white, 0);
    cam.pushFrame(1, black, 0);
    ComposedFrame f = comp.compose(in);
    CHECK(at(*f.pixels, 3, 3).r > 200);    // trail still visible (255 * 7/8 = 223)
    comp.release(f);
    comp.setQualityStep(1);                // shed trails
    f = comp.compose(in);                  // applies shed flag to the camera layer
    comp.release(f);
    cam.pushFrame(1, white, 0);
    cam.pushFrame(1, black, 0);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 3, 3).r == 0);     // no trail while shed
    comp.release(f);
    comp.setQualityStep(0);
    f = comp.compose(in);
    comp.release(f);
    cam.pushFrame(1, white, 0);
    cam.pushFrame(1, black, 0);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 3, 3).r > 200);    // restored
    comp.release(f);
}

TEST_CASE("REQ-S09: preview does not publish; publication is an explicit flag") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12).ok());
    CameraLayerConfig cfg;
    cfg.sourceId = 7;
    cfg.published = false;
    CameraLayer cam = makeCam(cfg);
    const PixelBuffer scene = solid(16, 12, Px{0, 0, 0, 255});
    const PixelBuffer frame = solid(16, 12, Px{0, 255, 0, 255});
    REQUIRE(cam.pushFrame(7, frame, 0));
    REQUIRE(cam.preview() != nullptr);                          // operator preview works
    CHECK(at(*cam.preview(), 0, 0) == Px{0, 255, 0, 255});
    FrameInputs in = sceneOnly(&scene);
    in.camera = &cam;
    ComposedFrame f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 0, 0, 255}));               // not on the audience composition
    comp.release(f);
    cam.setPublished(true);
    f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 255, 0, 255}));
    comp.release(f);
}

TEST_CASE("REQ-S10: treatments affect only the routed camera layer") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12).ok());
    const PixelBuffer scene = coords(16, 12);
    const PixelBuffer cam1 = solid(16, 12, Px{200, 100, 50, 255});
    CameraLayerConfig plain;
    plain.sourceId = 1;
    plain.published = true;
    plain.placement = CameraPlacement::Pip;
    plain.pip = RectF{0.5f, 0.5f, 0.5f, 0.5f};
    CameraLayerConfig treated = plain;
    treated.treatments.mirror = true;
    treated.treatments.tintOn = true;
    treated.treatments.tintColor = Rgb{0, 0, 255};
    treated.treatments.tintAmount = 1.f;
    treated.treatments.posterizeOn = true;
    treated.treatments.thresholdOn = true;
    CameraLayer a = makeCam(plain), b = makeCam(treated);
    a.pushFrame(1, cam1, 0);
    b.pushFrame(1, cam1, 0);
    FrameInputs ia = sceneOnly(&scene), ib = sceneOnly(&scene);
    ia.camera = &a;
    ib.camera = &b;
    ComposedFrame fa = comp.compose(ia);
    const PixelBuffer ra = copyOf(fa);
    comp.release(fa);
    ComposedFrame fb = comp.compose(ib);
    const PixelBuffer rb = copyOf(fb);
    comp.release(fb);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            const bool inPip = x >= 8 && y >= 6;
            if (!inPip) {
                CHECK(at(ra, x, y) == at(rb, x, y));
                CHECK(at(ra, x, y) == at(scene, x, y));   // scene untouched by either
            }
        }
    CHECK(at(ra, 12, 9) == Px{200, 100, 50, 255});
    CHECK(at(rb, 12, 9) == Px{0, 0, 255, 255});
}

TEST_CASE("REQ-S10: PIP uses a normalised rectangle; split-screen has fit/crop controls") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12).ok());
    const PixelBuffer scene = solid(16, 12, Px{255, 0, 0, 255});
    const PixelBuffer frame = solid(8, 6, Px{0, 255, 0, 255});
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.published = true;
    cfg.placement = CameraPlacement::Pip;
    cfg.pip = RectF{0.25f, 0.25f, 0.5f, 0.5f};
    cfg.fit = FitMode::Stretch;
    CameraLayer cam = makeCam(cfg);
    cam.pushFrame(1, frame, 0);
    FrameInputs in = sceneOnly(&scene);
    in.camera = &cam;
    ComposedFrame f = comp.compose(in);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            const bool inside = x >= 4 && x < 12 && y >= 3 && y < 9;
            CHECK(at(*f.pixels, x, y) == (inside ? Px{0, 255, 0, 255} : Px{255, 0, 0, 255}));
        }
    comp.release(f);

    // split: camera left half; the scene (16x12) is fitted in the right 8x12 pane
    cfg.placement = CameraPlacement::Split;
    cfg.split.vertical = true;
    cfg.split.cameraFirst = true;
    cfg.split.ratio = 0.5f;
    cfg.split.fit = FitMode::Crop;
    cam.configure(cfg);
    CompositorConfig cc;
    cc.background = Rgb{0, 0, 80};
    cc.sceneFit = FitMode::Fit;
    comp.setConfig(cc);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 0, 0) == Px{0, 255, 0, 255});
    CHECK(at(*f.pixels, 7, 11) == Px{0, 255, 0, 255});
    CHECK(at(*f.pixels, 12, 6) == Px{255, 0, 0, 255});      // scene content in the right pane
    CHECK(at(*f.pixels, 12, 0) == Px{0, 0, 80, 255});       // fit letterbox bar (8x6 picture centred in 8x12)
    CHECK(at(*f.pixels, 12, 11) == Px{0, 0, 80, 255});
    comp.release(f);
    cc.sceneFit = FitMode::Crop;                            // crop control: pane completely filled
    comp.setConfig(cc);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 12, 0) == Px{255, 0, 0, 255});
    CHECK(at(*f.pixels, 15, 11) == Px{255, 0, 0, 255});
    comp.release(f);
    cfg.split.cameraFirst = false;                          // camera on the right
    cam.configure(cfg);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 15, 0) == Px{0, 255, 0, 255});
    CHECK(at(*f.pixels, 0, 0) == Px{255, 0, 0, 255});
    comp.release(f);
    cfg.split.vertical = false;                             // stacked, camera at the bottom
    cam.configure(cfg);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 5, 11) == Px{0, 255, 0, 255});
    CHECK(at(*f.pixels, 5, 0) == Px{255, 0, 0, 255});
    comp.release(f);
}

TEST_CASE("REQ-S11: lost camera freezes for <= 500 ms then hides (fake clock)") {
    ss::FakeClock clk(0);
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.published = true;
    cfg.freezeMs = 9999;   // clamped to 500
    cfg.staleMs = 250;
    CameraLayer cam = makeCam(cfg);
    const PixelBuffer fr = solid(16, 12, Px{10, 20, 30, 255});
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::NoSignal);
    REQUIRE(cam.pushFrame(1, fr, clk.nowMonotonic()));
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Live);
    clk.advance(msToNs(249));
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Live);
    clk.advance(msToNs(1));    // t=250: loss established, freeze window opens
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Frozen);
    CHECK(cam.visible());
    CHECK(at(*cam.frame(), 3, 3) == Px{10, 20, 30, 255});
    clk.advance(msToNs(499));  // t=749: 499 ms into the loss
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Frozen);
    clk.advance(msToNs(1));    // t=750: 500 ms into the loss
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Hidden);
    CHECK(!cam.visible());
    // the same device coming back resumes immediately
    REQUIRE(cam.pushFrame(1, fr, clk.nowMonotonic()));
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Live);
}

TEST_CASE("REQ-S11: explicit device-lost event, shorter freeze, placeholder policy") {
    ss::FakeClock clk(secToNs(10));
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.published = true;
    cfg.freezeMs = 100;
    cfg.lossPolicy = LossPolicy::Placeholder;
    cfg.placeholderColor = Rgb{40, 0, 40};
    CameraLayer cam = makeCam(cfg);
    const PixelBuffer fr = solid(16, 12, Px{10, 20, 30, 255});
    cam.pushFrame(1, fr, clk.nowMonotonic());
    cam.markLost(clk.nowMonotonic());
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Frozen);
    clk.advance(msToNs(99));
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Frozen);
    clk.advance(msToNs(1));
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Placeholder);
    CHECK(cam.visible());

    Compositor comp;
    REQUIRE(comp.prepare(64, 48).ok());
    const PixelBuffer scene = solid(64, 48, Px{255, 0, 0, 255});
    FrameInputs in = sceneOnly(&scene);
    in.camera = &cam;
    in.nowNs = clk.nowMonotonic();
    ComposedFrame f = comp.compose(in);
    CHECK(at(*f.pixels, 1, 1) == Px{40, 0, 40, 255});        // placeholder fills the (Full) layer
    comp.release(f);

    cfg.freezeMs = 0;                                         // zero freeze: straight to policy
    cam.configure(cfg);
    CHECK(cam.update(clk.nowMonotonic()) == CameraState::Placeholder);
    cfg.lossPolicy = LossPolicy::Hide;
    cam.configure(cfg);
    f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{255, 0, 0, 255}));          // hidden: the scene alone
    comp.release(f);
}

TEST_CASE("REQ-S11: no automatic alternate-camera selection; foreign sources are rejected and counted") {
    ss::FakeClock clk(0);
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.published = true;
    cfg.freezeMs = 0;
    CameraLayer cam = makeCam(cfg);
    const PixelBuffer fr = solid(16, 12, Px{10, 20, 30, 255});
    cam.pushFrame(1, fr, 0);
    cam.markLost(0);
    CHECK(cam.update(0) == CameraState::Hidden);
    const PixelBuffer other = solid(16, 12, Px{99, 99, 99, 255});
    CHECK(!cam.pushFrame(2, other, msToNs(10)));          // a different camera does not take over
    CHECK(!cam.pushFrame(0, other, msToNs(10)));
    CHECK(cam.rejectedFrames() == 2);
    CHECK(cam.update(msToNs(10)) == CameraState::Hidden);
    CHECK(cam.config().sourceId == 1);
    // oversized and empty frames are also rejected without allocating
    CHECK(!cam.pushFrame(1, solid(17, 12, Px{1, 1, 1, 255}), msToNs(11)));
    CHECK(!cam.pushFrame(1, PixelBuffer(), msToNs(11)));
    CHECK(cam.acceptedFrames() == 1);
}

TEST_CASE("REQ-S11: lost camera that never produced a frame shows nothing to freeze") {
    CameraLayerConfig cfg;
    cfg.sourceId = 1;
    cfg.published = true;
    cfg.lossPolicy = LossPolicy::Hide;
    CameraLayer cam = makeCam(cfg);
    cam.markLost(0);
    CHECK(cam.update(0) == CameraState::Hidden);
    CHECK(cam.frame() == nullptr);
}
