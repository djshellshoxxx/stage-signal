// Counting operator new: proves the steady-state render path allocates nothing after prepare().
#include <atomic>
#include <cstdlib>
#include <new>
#include "util.hpp"

namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_count{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace tu;

TEST_CASE("allocation counter sanity (the zero-allocation checks below are meaningful)") {
    g_allocs = 0;
    g_count = true;
    std::vector<int>* v = new std::vector<int>(10);
    g_count = false;
    CHECK(g_allocs.load() >= 2);
    delete v;
}

TEST_CASE("REQ-D04/section7: no allocation after prepare() across the full render path") {
    const PixelBuffer a = coords(16, 12), b = solid(40, 30, Px{0, 90, 0, 255});
    const PixelBuffer media = solid(8, 8, Px{0, 255, 0, 255});
    const PixelBuffer camFrame = coords(16, 12);

    Compositor comp;
    REQUIRE(comp.prepare(64, 48, 0.75f, 3).ok());
    CompositorConfig cfg;
    cfg.policy = OutputPolicy{true, true, true, true};
    comp.setConfig(cfg);

    CameraLayerConfig cc;
    cc.sourceId = 1; cc.published = true; cc.placement = CameraPlacement::Split;
    cc.treatments.mirror = true; cc.treatments.posterizeOn = true; cc.treatments.thresholdOn = true;
    cc.treatments.tintOn = true; cc.treatments.trailFrames = 6;
    CameraLayer cam;
    REQUIRE(cam.prepare(16, 12).ok());
    cam.configure(cc);

    TextLayer tl;
    TextItem t;
    t.id = 1; t.utf8 = "hello"; t.bitmap = sharedSolid(20, 4, Px{255, 255, 255, 255}); t.size = 0.2f;
    REQUIRE(tl.publish(t, 0).ok());
    t.id = 2; t.scroll = true; t.durationSec = 6;
    REQUIRE(tl.publish(t, 0).ok());

    OverlayInfo ov;
    ov.tempo.bpm = 127.5; ov.tempo.source = ss::ValueSource::Estimated;
    ov.key.tonic = 4; ov.key.source = ss::ValueSource::Manual;
    ov.clipWarning = true;
    ov.setBrandingText("MSR");
    ov.addDiagnostic("FPS 30");
    ov.addDiagnostic("/home/x");

    std::shared_ptr<const PixelBuffer> unusedKeepAlive = tl.activeCount(0) ? nullptr : nullptr;
    (void)unusedKeepAlive;

    g_allocs = 0;
    g_count = true;
    for (int i = 0; i < 60; ++i) {
        cam.pushFrame(1, camFrame, msToNs(i * 33));
        if (i == 20) comp.setRotation(90);
        if (i == 30) comp.setRotation(0);
        if (i == 35) comp.setInternalScale(0.5f);
        if (i == 40) comp.setQualityStep(3);
        if (i == 45) comp.setQualityStep(0);
        if (i == 50) comp.setVisualBlackout(true);
        if (i == 55) comp.setVisualBlackout(false);
        FrameInputs in = sceneOnly(&a);
        in.scene.b = &b;
        in.scene.progress = (i % 20) / 19.f;
        in.scene.kind = static_cast<ss::model::TransitionKind>(i % 3);
        in.mediaCount = 1;
        in.media[0].frame = &media;
        in.media[0].opacity = 0.7f;
        in.camera = &cam;
        in.text = &tl;
        in.overlay = &ov;
        in.nowNs = msToNs(i * 33);
        in.masterBrightness = 0.9f;
        in.idle = (i % 17) == 16;
        ComposedFrame f = comp.compose(in);
        if (f.ok()) comp.release(f);
        // a dropped frame must not allocate either
        ComposedFrame g1 = comp.compose(in), g2 = comp.compose(in), g3 = comp.compose(in), g4 = comp.compose(in);
        comp.release(g1); comp.release(g2); comp.release(g3);
        (void)g4;
    }
    g_count = false;
    CHECK(g_allocs.load() == 0);
    CHECK(comp.droppedFrames() > 0);
}

TEST_CASE("no allocation while the camera layer processes and loses frames") {
    CameraLayer cam;
    REQUIRE(cam.prepare(32, 24).ok());
    CameraLayerConfig cc;
    cc.sourceId = 2;
    cc.treatments.trailFrames = 8; cc.treatments.mirror = true; cc.treatments.tintOn = true;
    cc.treatments.thresholdOn = true; cc.treatments.posterizeOn = true;
    cam.configure(cc);
    const PixelBuffer f1 = coords(16, 12), f2 = solid(32, 24, Px{1, 2, 3, 255}), foreign = coords(16, 12);
    g_allocs = 0;
    g_count = true;
    for (int i = 0; i < 50; ++i) {
        cam.pushFrame(2, (i % 2) ? f1 : f2, msToNs(i));
        cam.pushFrame(3, foreign, msToNs(i));
        cam.update(msToNs(i * 100));
        cam.configure(cc);
    }
    g_count = false;
    CHECK(g_allocs.load() == 0);
}

TEST_CASE("pool frames keep stable storage addresses (no reallocation across frames)") {
    Compositor comp;
    REQUIRE(comp.prepare(16, 12, 1.f, 2).ok());
    const PixelBuffer scene = coords(16, 12);
    ComposedFrame a = comp.compose(sceneOnly(&scene));
    const std::uint8_t* addr = a.pixels->rgba.data();
    comp.release(a);
    for (int i = 0; i < 10; ++i) {
        comp.setRotation(i % 2 ? 0 : 180);
        ComposedFrame f = comp.compose(sceneOnly(&scene));
        comp.release(f);
    }
    ComposedFrame b = comp.compose(sceneOnly(&scene));
    CHECK(b.pixels->rgba.data() == addr);
    comp.release(b);
}
