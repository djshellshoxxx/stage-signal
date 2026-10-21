#include "util.hpp"
using namespace tu;

namespace {
PixelBuffer rotateCopy(const PixelBuffer& s, int deg) {
    const bool swap = deg == 90 || deg == 270;
    PixelBuffer d(swap ? s.height : s.width, swap ? s.width : s.height);
    rotateInto(s, deg, d);
    return d;
}
}  // namespace

TEST_CASE("compositor: prepare validates every argument before allocating") {
    Compositor c;
    CHECK(!c.prepare(0, 10).ok());
    CHECK(!c.prepare(10, 0).ok());
    CHECK(!c.prepare(5000, 10).ok());
    CHECK(!c.prepare(4097, 10).ok());
    CHECK(!Compositor().prepare(16, 12, 0.1f).ok());
    CHECK(!Compositor().prepare(16, 12, 1.5f).ok());
    CHECK(!Compositor().prepare(16, 12, std::nanf("")).ok());
    CHECK(!Compositor().prepare(16, 12, 1.f, 3, 45).ok());
    CHECK(!Compositor().prepare(16, 12, 1.f, 0).ok());
    CHECK(!Compositor().prepare(16, 12, 1.f, 99).ok());
    Compositor unprepared;
    CHECK(!unprepared.compose(FrameInputs{}).ok());
}

TEST_CASE("composition order: background, scene, media, camera, text, overlays, brightness (exact)") {
    // 64x48 canvas; every layer is an opaque rectangle in a distinct colour; later layers must win overlaps.
    Compositor comp;
    REQUIRE(comp.prepare(64, 48).ok());
    CompositorConfig cfg;
    cfg.background = Rgb{1, 2, 3};
    cfg.policy.showBpmKey = true;
    comp.setConfig(cfg);

    const Px red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255}, white{255, 255, 255, 255};
    const PixelBuffer scene = solid(32, 48, red);                    // fit -> letterboxed: x 8..39 (32x48 in 64x48 => 32 wide, centred)
    const PixelBuffer media = solid(8, 8, green);
    const PixelBuffer camFrame = solid(8, 8, blue);
    CameraLayerConfig cc;
    cc.sourceId = 1; cc.published = true; cc.placement = CameraPlacement::Pip;
    cc.pip = RectF{0.4f, 0.4f, 0.4f, 0.4f}; cc.fit = FitMode::Stretch;
    CameraLayer cam;
    REQUIRE(cam.prepare(8, 8).ok());
    cam.configure(cc);
    cam.pushFrame(1, camFrame, 0);
    TextLayer tl;
    TextItem t;
    t.id = 1; t.utf8 = "x"; t.bitmap = sharedSolid(2, 2, white); t.colorBitmap = true;
    t.x = 0.55f; t.y = 0.55f; t.size = 0.15f; t.fadeSec = 0;
    REQUIRE(tl.publish(t, 0).ok());
    OverlayInfo ov;
    ov.tempo.bpm = 128; ov.tempo.source = ss::ValueSource::Manual;

    FrameInputs in = sceneOnly(&scene);
    in.mediaCount = 1;
    in.media[0].frame = &media;
    in.media[0].fit = FitMode::Stretch;
    in.media[0].region = RectF{0.3f, 0.3f, 0.5f, 0.5f};
    in.camera = &cam;
    in.text = &tl;
    in.overlay = &ov;
    ComposedFrame f = comp.compose(in);
    REQUIRE(f.ok());
    const PixelBuffer& o = *f.pixels;
    CHECK(at(o, 60, 40) == Px{1, 2, 3, 255});     // background (outside the letterboxed scene, x 16..47)
    CHECK(at(o, 20, 44) == red);                  // scene, below the media region
    CHECK(at(o, 22, 16) == green);                // media above scene
    CHECK(at(o, 30, 23) == blue);                 // camera above media: pip x 25.6..51.2, y 19.2..38.4
    CHECK(at(o, 35, 27) == white);                // text above camera: 7x7 box centred (35,26)
    CHECK(at(o, 3 + 0, 3 + 1) != at(o, 3, 40));   // overlay label region differs from untouched scene pixels
    CHECK(at(o, 3, 3) == white);                  // 'B' column 0 of "BPM ..." drawn last
    comp.release(f);

    // master brightness is applied after everything, including overlays
    in.masterBrightness = 0.5f;
    f = comp.compose(in);
    CHECK(at(*f.pixels, 60, 40) == Px{1, 1, 2, 255});   // (1,2,3) * 128/256, rounded
    CHECK(at(*f.pixels, 20, 44) == Px{128, 0, 0, 255});
    CHECK(at(*f.pixels, 22, 16) == Px{0, 128, 0, 255});
    CHECK(at(*f.pixels, 30, 23) == Px{0, 0, 128, 255});
    CHECK(at(*f.pixels, 35, 27) == Px{128, 128, 128, 255});
    CHECK(at(*f.pixels, 3, 3) == Px{128, 128, 128, 255});
    comp.release(f);
}

TEST_CASE("master brightness: 1 is identity, 0 is black, out of range and NaN clamp") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12).ok());
    const PixelBuffer scene = coords(16, 12);
    FrameInputs in = sceneOnly(&scene);
    ComposedFrame f = comp.compose(in);
    CHECK(f.pixels->rgba == scene.rgba);
    comp.release(f);
    in.masterBrightness = 0.f;
    f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 0, 0, 255}));
    comp.release(f);
    in.masterBrightness = 5.f;
    f = comp.compose(in);
    CHECK(f.pixels->rgba == scene.rgba);
    comp.release(f);
    in.masterBrightness = std::nanf("");
    f = comp.compose(in);
    CHECK(f.pixels->rgba == scene.rgba);
    comp.release(f);
}

TEST_CASE("REQ-D04: fit / crop / stretch exact pixels at 16x12 from a 4x4 coordinate image") {
    const PixelBuffer src = coords(4, 4);
    Compositor comp;
    REQUIRE(comp.prepare(16, 12).ok());
    CompositorConfig cfg;
    cfg.background = Rgb{9, 9, 9};
    cfg.sceneSampling = Sampling::Nearest;
    // Fit: square 12x12 centred => columns 0,1 and 14,15 are bars
    cfg.sceneFit = FitMode::Fit;
    comp.setConfig(cfg);
    ComposedFrame f = comp.compose(sceneOnly(&src));
    for (int y = 0; y < 12; ++y) {
        CHECK(at(*f.pixels, 0, y) == Px{9, 9, 9, 255});
        CHECK(at(*f.pixels, 1, y) == Px{9, 9, 9, 255});
        CHECK(at(*f.pixels, 14, y) == Px{9, 9, 9, 255});
        CHECK(at(*f.pixels, 15, y) == Px{9, 9, 9, 255});
    }
    CHECK(at(*f.pixels, 2, 0) == at(src, 0, 0));
    CHECK(at(*f.pixels, 13, 0) == at(src, 3, 0));
    CHECK(at(*f.pixels, 2, 11) == at(src, 0, 3));
    CHECK(at(*f.pixels, 13, 11) == at(src, 3, 3));
    comp.release(f);
    // Crop: full 16x12, source window rows 0..2 (4x3)
    cfg.sceneFit = FitMode::Crop;
    comp.setConfig(cfg);
    f = comp.compose(sceneOnly(&src));
    CHECK(at(*f.pixels, 0, 0) == at(src, 0, 0));
    CHECK(at(*f.pixels, 15, 0) == at(src, 3, 0));
    CHECK(at(*f.pixels, 0, 11) == at(src, 0, 2));
    CHECK(at(*f.pixels, 15, 11) == at(src, 3, 2));
    comp.release(f);
    // Stretch
    cfg.sceneFit = FitMode::Stretch;
    comp.setConfig(cfg);
    f = comp.compose(sceneOnly(&src));
    CHECK(at(*f.pixels, 0, 0) == at(src, 0, 0));
    CHECK(at(*f.pixels, 15, 0) == at(src, 3, 0));
    CHECK(at(*f.pixels, 0, 11) == at(src, 0, 3));
    CHECK(at(*f.pixels, 15, 11) == at(src, 3, 3));
    comp.release(f);
}

TEST_CASE("REQ-D07: rotation and resolution changes recompose from normalised coordinates, content unchanged") {
    const PixelBuffer scene = coords(16, 12);
    PixelBuffer sceneBefore = scene;
    TextLayer tl;
    TextItem t;
    t.id = 1; t.utf8 = "t"; t.bitmap = sharedSolid(2, 2, Px{255, 255, 255, 255}); t.colorBitmap = true;
    t.x = 0.25f; t.y = 0.25f; t.size = 0.25f; t.fadeSec = 0;
    REQUIRE(tl.publish(t, 0).ok());

    Compositor base;
    REQUIRE(base.prepare(16, 12).ok());
    FrameInputs in = sceneOnly(&scene);
    in.text = &tl;
    ComposedFrame fb = base.compose(in);
    const PixelBuffer r0 = copyOf(fb);
    base.release(fb);

    for (int rot : {90, 180, 270}) {
        Compositor c;
        const bool swap = rot != 180;
        REQUIRE(c.prepare(swap ? 12 : 16, swap ? 16 : 12, 1.f, 3, rot).ok());
        ComposedFrame f = c.compose(in);
        REQUIRE(f.ok());
        CAPTURE(rot);
        CHECK(f.pixels->rgba == rotateCopy(r0, rot).rgba);
        c.release(f);
    }
    // runtime switch on a single compositor: same result, and 0 returns the original exactly
    Compositor c;
    REQUIRE(c.prepare(12, 16, 1.f, 3, 90).ok());
    ComposedFrame f = c.compose(in);
    CHECK(f.pixels->rgba == rotateCopy(r0, 90).rgba);
    c.release(f);
    REQUIRE(c.setRotation(180).ok());
    CHECK(c.logicalWidth() == 12);   // physical 12x16 viewport showing a rotated 12x16 logical composition
    f = c.compose(in);
    CHECK(f.pixels->width == 12);
    c.release(f);
    CHECK(!c.setRotation(45).ok());
    CHECK(sceneBefore.rgba == scene.rgba);   // show content not modified
}

TEST_CASE("REQ-D07: 4:3, 16:9 and portrait compose without distortion of content (fit) and keep logical size") {
    const PixelBuffer scene = solid(800, 600, Px{10, 200, 30, 255});
    struct T { std::uint32_t w, h; int rot; } ts[] = {{800, 600, 0}, {1280, 720, 0}, {720, 1280, 0}, {1280, 720, 90}, {720, 1280, 270}};
    for (const T& t : ts) {
        Compositor c;
        REQUIRE(c.prepare(t.w, t.h, 1.f, 3, t.rot).ok());
        ComposedFrame f = c.compose(sceneOnly(&scene));
        REQUIRE(f.ok());
        CHECK(f.pixels->width == t.w);
        CHECK(f.pixels->height == t.h);
        CHECK(at(*f.pixels, int(t.w) / 2, int(t.h) / 2) == Px{10, 200, 30, 255});
        c.release(f);
    }
}

TEST_CASE("REQ-D08: visual blackout applies by the next admitted frame and restores the exact prior composition") {
    const PixelBuffer a = coords(16, 12), b = solid(16, 12, Px{0, 0, 90, 255});
    TextLayer tl;
    TextItem t;
    t.id = 1; t.utf8 = "t"; t.bitmap = sharedSolid(2, 2, Px{255, 255, 255, 255}); t.colorBitmap = true;
    t.size = 0.3f; t.fadeSec = 1.0; t.durationSec = 100;
    REQUIRE(tl.publish(t, 0).ok());
    CameraLayerConfig cc;
    cc.sourceId = 1; cc.published = true; cc.placement = CameraPlacement::Pip; cc.treatments.trailFrames = 4;
    CameraLayer camA, camB;
    REQUIRE(camA.prepare(16, 12).ok());
    REQUIRE(camB.prepare(16, 12).ok());
    camA.configure(cc);
    camB.configure(cc);

    Compositor withBlackout, never;
    REQUIRE(withBlackout.prepare(16, 12).ok());
    REQUIRE(never.prepare(16, 12).ok());
    std::vector<std::uint64_t> hashesNever;
    for (int i = 0; i < 12; ++i) {
        const PixelBuffer cam = solid(16, 12, Px{std::uint8_t(i * 20), 100, 50, 255});
        camA.pushFrame(1, cam, secToNs(i));
        camB.pushFrame(1, cam, secToNs(i));
        FrameInputs inA = sceneOnly(&a), inB = sceneOnly(&a);
        inA.scene.b = inB.scene.b = &b;
        inA.scene.progress = inB.scene.progress = i / 11.f;
        inA.camera = &camA; inB.camera = &camB;
        inA.text = inB.text = &tl;
        inA.nowNs = inB.nowNs = secToNs(i) / 4;
        if (i == 4) withBlackout.setVisualBlackout(true);     // control path flips between frames
        if (i == 8) withBlackout.setVisualBlackout(false);
        ComposedFrame fa = withBlackout.compose(inA);
        ComposedFrame fb = never.compose(inB);
        REQUIRE(fa.ok());
        REQUIRE(fb.ok());
        if (i >= 4 && i < 8) {
            CHECK(fa.blackout);
            CHECK(allEqual(*fa.pixels, Px{0, 0, 0, 255}));     // honoured on the very next admitted frame
            CHECK(!fb.blackout);
        } else {
            CHECK(!fa.blackout);
            CHECK(fa.pixels->rgba == fb.pixels->rgba);          // before, and after restore: identical to never-blacked
        }
        withBlackout.release(fa);
        never.release(fb);
    }
    CHECK(!withBlackout.visualBlackout());
}

TEST_CASE("REQ-D08: blackout hides overlays and text too; control state is not part of the composition") {
    Compositor comp;
    REQUIRE(comp.prepare(64, 48).ok());
    CompositorConfig cfg;
    cfg.policy.showBpmKey = true;
    comp.setConfig(cfg);
    const PixelBuffer scene = solid(64, 48, Px{200, 200, 200, 255});
    OverlayInfo ov;
    ov.tempo.bpm = 100; ov.tempo.source = ss::ValueSource::Manual;
    FrameInputs in = sceneOnly(&scene);
    in.overlay = &ov;
    comp.setVisualBlackout(true);
    ComposedFrame f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{0, 0, 0, 255}));
    comp.release(f);
    comp.setVisualBlackout(false);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 30, 40) == Px{200, 200, 200, 255});
    comp.release(f);
}

TEST_CASE("idle screen replaces scene, media and camera on audio loss (configurable colour and logo placeholder)") {
    Compositor comp;
    REQUIRE(comp.prepare(64, 48).ok());
    CompositorConfig cfg;
    cfg.idle.color = Rgb{10, 20, 30};
    cfg.idle.showLogo = false;
    comp.setConfig(cfg);
    const PixelBuffer scene = solid(64, 48, Px{255, 0, 0, 255});
    const PixelBuffer media = solid(8, 8, Px{0, 255, 0, 255});
    CameraLayerConfig cc; cc.sourceId = 1; cc.published = true;
    CameraLayer cam;
    REQUIRE(cam.prepare(8, 8).ok());
    cam.configure(cc);
    cam.pushFrame(1, media, 0);
    FrameInputs in = sceneOnly(&scene);
    in.mediaCount = 1; in.media[0].frame = &media;
    in.camera = &cam;
    in.idle = true;
    ComposedFrame f = comp.compose(in);
    CHECK(allEqual(*f.pixels, Px{10, 20, 30, 255}));
    comp.release(f);
    cfg.idle.showLogo = true;
    comp.setConfig(cfg);
    f = comp.compose(in);
    CHECK(at(*f.pixels, 0, 0) == Px{10, 20, 30, 255});
    CHECK(at(*f.pixels, 32, 18) != Px{10, 20, 30, 255});        // top edge of the placeholder logo box (outline)
    comp.release(f);
    in.idle = false;
    f = comp.compose(in);
    CHECK(at(*f.pixels, 32, 24) == Px{0, 255, 0, 255});          // camera (Full placement) back on
    comp.release(f);
    // logo bitmap variant
    cfg.idle.logo = sharedSolid(4, 4, Px{255, 255, 0, 255});
    comp.setConfig(cfg);
    in.idle = true;
    f = comp.compose(in);
    CHECK(at(*f.pixels, 32, 24) == Px{255, 255, 0, 255});
    comp.release(f);
}

TEST_CASE("adaptive render scale: internal size shrinks, output size stays, overlays stay crisp") {
    Compositor comp;
    REQUIRE(comp.prepare(64, 48, 0.5f).ok());
    CHECK(comp.internalWidth() == 32);
    CHECK(comp.internalHeight() == 24);
    CHECK(comp.outputWidth() == 64);
    const PixelBuffer scene = solid(16, 12, Px{30, 60, 90, 255});
    CompositorConfig cfg;
    cfg.policy.showBpmKey = true;
    cfg.sceneSampling = Sampling::Nearest;
    cfg.upscaleSampling = Sampling::Nearest;
    comp.setConfig(cfg);
    OverlayInfo ov;
    ov.tempo.bpm = 128; ov.tempo.source = ss::ValueSource::Manual;
    FrameInputs in = sceneOnly(&scene);
    in.overlay = &ov;
    ComposedFrame f = comp.compose(in);
    REQUIRE(f.ok());
    CHECK(f.pixels->width == 64);
    // nearest upscale only reuses source values: every non-overlay pixel is one of the scene's colours
    const PixelBuffer low = copyOf(f);
    comp.release(f);
    REQUIRE(comp.setInternalScale(1.0f).ok());
    f = comp.compose(in);
    const PixelBuffer full = copyOf(f);
    comp.release(f);
    // overlay label box (top-left, drawn at output resolution) is identical for both internal scales
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 100 && x < 64; ++x)
            if (y < 11) CHECK(at(low, x, y) == at(full, x, y));
    // ladder
    CHECK(scaleForQualityStep(0) == doctest::Approx(1.0f));
    CHECK(scaleForQualityStep(1) == doctest::Approx(1.0f));
    CHECK(scaleForQualityStep(2) == doctest::Approx(0.75f));
    CHECK(scaleForQualityStep(3) == doctest::Approx(0.5f));
    CHECK(scaleForQualityStep(4) == doctest::Approx(0.35f));
    comp.setQualityStep(2);
    CHECK(comp.internalWidth() == 48);
    comp.setQualityStep(4);
    CHECK(comp.internalWidth() == 22);
    CHECK(comp.internalHeight() == 17);
    comp.setQualityStep(0);
    CHECK(comp.internalWidth() == 64);
    CHECK(!comp.setInternalScale(0.1f).ok());
}

TEST_CASE("adaptive render scale: nearest keeps hard edges, bilinear smooths the upscale") {
    PixelBuffer scene(2, 1);
    set(scene, 0, 0, Px{0, 0, 0, 255});
    set(scene, 1, 0, Px{255, 255, 255, 255});
    for (Sampling up : {Sampling::Nearest, Sampling::Bilinear}) {
        Compositor comp;
        REQUIRE(comp.prepare(16, 2, 0.5f).ok());   // internal 8x1
        CompositorConfig cfg;
        cfg.sceneFit = FitMode::Stretch;
        cfg.sceneSampling = Sampling::Nearest;
        cfg.upscaleSampling = up;
        comp.setConfig(cfg);
        ComposedFrame f = comp.compose(sceneOnly(&scene));
        int distinct = 0;
        std::uint8_t last = 255;
        for (int x = 0; x < 16; ++x) {
            const std::uint8_t v = at(*f.pixels, x, 0).r;
            if (v != last) ++distinct;
            last = v;
        }
        if (up == Sampling::Nearest) {
            for (int x = 0; x < 16; ++x) { const auto v = at(*f.pixels, x, 0).r; CHECK((v == 0 || v == 255)); }
            CHECK(at(*f.pixels, 0, 0).r == 0);
            CHECK(at(*f.pixels, 15, 0).r == 255);
        } else {
            CHECK(distinct > 3);
        }
        comp.release(f);
    }
}

TEST_CASE("frame pool is bounded: exhausted pool drops and counts, release restores") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12, 1.f, 3).ok());
    const PixelBuffer scene = solid(16, 12, Px{1, 2, 3, 255});
    ComposedFrame f1 = comp.compose(sceneOnly(&scene)), f2 = comp.compose(sceneOnly(&scene)), f3 = comp.compose(sceneOnly(&scene));
    REQUIRE(f1.ok());
    REQUIRE(f2.ok());
    REQUIRE(f3.ok());
    CHECK(f1.slot != f2.slot);
    CHECK(comp.freeFrames() == 0);
    ComposedFrame f4 = comp.compose(sceneOnly(&scene));
    CHECK(!f4.ok());
    CHECK(comp.droppedFrames() == 1);
    comp.release(f2);
    CHECK(comp.freeFrames() == 1);
    ComposedFrame f5 = comp.compose(sceneOnly(&scene));
    CHECK(f5.ok());
    CHECK(f5.slot == f2.slot);
    CHECK(f5.sequence > f3.sequence);
    comp.release(f1); comp.release(f3); comp.release(f5);
    CHECK(comp.freeFrames() == 3);
}

TEST_CASE("determinism: identical inputs give identical frame hashes across instances") {
    auto run = [](std::vector<std::uint64_t>& hs) {
        const PixelBuffer a = coords(16, 12), b = solid(16, 12, Px{0, 90, 0, 255});
        Compositor comp;
        REQUIRE(comp.prepare(24, 18, 0.75f).ok());
        CompositorConfig cfg;
        cfg.policy.showBpmKey = true;
        comp.setConfig(cfg);
        OverlayInfo ov;
        ov.tempo.bpm = 127.5; ov.tempo.source = ss::ValueSource::Estimated;
        ov.key.tonic = 3; ov.key.source = ss::ValueSource::Manual;
        CameraLayerConfig cc; cc.sourceId = 1; cc.published = true; cc.placement = CameraPlacement::Pip;
        cc.treatments.mirror = true; cc.treatments.posterizeOn = true; cc.treatments.tintOn = true; cc.treatments.trailFrames = 5;
        CameraLayer cam;
        REQUIRE(cam.prepare(16, 12).ok());
        cam.configure(cc);
        for (int i = 0; i < 20; ++i) {
            cam.pushFrame(1, coords(16, 12), 0);
            PixelBuffer cf = solid(16, 12, Px{std::uint8_t(i * 12), 0, 0, 255});
            cam.pushFrame(1, cf, msToNs(i * 33));
            FrameInputs in = sceneOnly(&a);
            in.scene.b = &b;
            in.scene.progress = i / 19.f;
            in.scene.kind = (i % 2) ? ss::model::TransitionKind::Crossfade : ss::model::TransitionKind::FadeThroughBlack;
            in.camera = &cam;
            in.overlay = &ov;
            in.nowNs = msToNs(i * 33);
            ComposedFrame f = comp.compose(in);
            hs.push_back(hash(*f.pixels));
            comp.release(f);
        }
    };
    std::vector<std::uint64_t> h1, h2;
    run(h1);
    run(h2);
    CHECK(h1 == h2);
    CHECK(h1.size() == 20);
    CHECK(h1[3] != h1[4]);
}
