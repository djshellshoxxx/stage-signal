// MOD-06 layer tests: camera treatments (REQ-S10), media fit modes, layer isolation, composition order.
#include "render_test_util.h"
#include <cmath>

using namespace rt;

namespace {
struct Rig {
  Renderer r; Frame out;
  Rig(int w = 320, int h = 240) { CHECK(r.configure(w, h, 1.0f).ok()); }
  void go(const RenderState& s) { r.render(out, s); }
};
RenderState camState(const Frame* cam) {
  RenderState s = blackState(); s.camera.frame = cam; s.camera.routed = true; s.camera.t.fit = FitMode::Stretch; return s;
}
int near(int a, int b, int tol) { return std::abs(a - b) <= tol; }
bool nearRgb(int a, int b, int tol) { return near(ch(a, 16), ch(b, 16), tol) && near(ch(a, 8), ch(b, 8), tol) && near(ch(a, 0), ch(b, 0), tol); }
}

TEST(camera_identity_and_mirror) {
  Frame cam = stripeFrame(64, 48); Rig g;
  RenderState s = camState(&cam); g.go(s);
  CHECK(px(g.out, 40, 100) == 0xFF0000 && px(g.out, 120, 100) == 0x00FF00 && px(g.out, 200, 100) == 0x0000FF && px(g.out, 280, 100) == 0xFFFFFF);
  s.camera.t.mirror = true; g.go(s);
  CHECK(px(g.out, 40, 100) == 0xFFFFFF && px(g.out, 120, 100) == 0x0000FF && px(g.out, 200, 100) == 0x00FF00 && px(g.out, 280, 100) == 0xFF0000);
}

TEST(camera_tint_0_to_1) {
  Frame cam = stripeFrame(64, 48); Rig g;
  RenderState s = camState(&cam); s.camera.t.tintRgb = 0x0000FF;
  s.camera.t.tintAmount = 0.0f; g.go(s); CHECK(px(g.out, 40, 100) == 0xFF0000);
  s.camera.t.tintAmount = 0.5f; g.go(s);
  CHECK(nearRgb(px(g.out, 40, 100), 0x80007F, 1)); CHECK(nearRgb(px(g.out, 120, 100), 0x00807F, 1)); CHECK(nearRgb(px(g.out, 200, 100), 0x0000FF, 1));
  s.camera.t.tintAmount = 1.0f; g.go(s);
  CHECK(px(g.out, 40, 100) == 0x0000FF && px(g.out, 120, 100) == 0x0000FF && px(g.out, 280, 100) == 0x0000FF);
  s.camera.t.tintAmount = 7.0f; g.go(s); CHECK(px(g.out, 40, 100) == 0x0000FF);   // clamped to 1
}

TEST(camera_threshold_around_adjustable_cutoff) {
  Frame cam = stripeFrame(64, 48); Rig g;
  RenderState s = camState(&cam); s.camera.t.threshold = true;
  s.camera.t.thresholdCutoff = 0.5f; g.go(s);          // luma R=77 G=150 B=29 W=255
  CHECK(px(g.out, 40, 100) == 0 && px(g.out, 120, 100) == 0xFFFFFF && px(g.out, 200, 100) == 0 && px(g.out, 280, 100) == 0xFFFFFF);
  s.camera.t.thresholdCutoff = 0.1f; g.go(s);
  CHECK(px(g.out, 40, 100) == 0xFFFFFF && px(g.out, 200, 100) == 0xFFFFFF);
  s.camera.t.thresholdCutoff = 0.9f; g.go(s);
  CHECK(px(g.out, 120, 100) == 0 && px(g.out, 280, 100) == 0xFFFFFF);
}

TEST(camera_posterize_2_to_16_levels) {
  Frame cam = solidFrame(64, 48, irgb(200, 100, 50)); Rig g;
  RenderState s = camState(&cam); s.camera.t.posterize = true;
  s.camera.t.posterizeLevels = 2; g.go(s); CHECK(px(g.out, 10, 10) == irgb(255, 0, 0));
  s.camera.t.posterizeLevels = 4; g.go(s); CHECK(px(g.out, 10, 10) == irgb(255, 85, 0));
  s.camera.t.posterizeLevels = 16; g.go(s); CHECK(px(g.out, 10, 10) == irgb(12 * 17, 6 * 17, 3 * 17));
  s.camera.t.posterizeLevels = 1; g.go(s); CHECK(px(g.out, 10, 10) == irgb(255, 0, 0));       // clamped to 2
  s.camera.t.posterizeLevels = 99; g.go(s); CHECK(px(g.out, 10, 10) == irgb(204, 102, 51));  // clamped to 16
  // distinct output levels of a ramp never exceed the requested count
  Frame ramp; ramp.resize(256, 4);
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 256; ++x) ramp.px[(size_t)y * 256 + x] = irgb(x, x, x);
  Rig g2(256, 64); RenderState s2 = camState(&ramp); s2.camera.t.posterize = true;
  for (int L = 2; L <= 16; ++L) {
    s2.camera.t.posterizeLevels = L; g2.go(s2);
    int distinct = 0; uint32_t last = 0xFFFFFFFF;
    for (int x = 0; x < 256; ++x) { uint32_t p = g2.out.px[10 * 256 + x]; if (p != last) { ++distinct; last = p; } }
    CHECK(distinct == L);
  }
}

TEST(camera_pip_rect_and_outside_unmodified) {
  Frame cam = stripeFrame(64, 48); Rig g;
  RenderState base = sceneState(7, 1, 700 * kNsPerMs, feats(0.4f, 0.4f, 0.4f, 0, false));
  Frame ref; g.go(base); ref = g.out;
  RenderState s = base; s.camera.frame = &cam; s.camera.routed = true;
  s.camera.t.pip = true; s.camera.t.pipRect = NormRect{0.5f, 0.5f, 0.25f, 0.25f};
  s.camera.t.mirror = true; s.camera.t.threshold = true; s.camera.t.tintAmount = 0.3f; s.camera.t.posterize = true; s.camera.t.trailFrames = 5;
  g.go(s);
  int inside = 0, changed = 0;
  for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) {
    bool in = x >= 160 && x < 240 && y >= 120 && y < 180;
    if (in) { ++inside; changed += g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]; }
    else if (g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]) { CHECK(false); return; }   // outside PIP: bit-identical
  }
  CHECK(inside == 80 * 60 && changed > inside / 2);
  // not routed => ignored completely even with extreme treatments
  s.camera.routed = false; g.go(s); CHECK(g.out.px == ref.px);
  // routed but null / invalid frame => ignored
  s.camera.routed = true; s.camera.frame = nullptr; g.go(s); CHECK(g.out.px == ref.px);
  Frame bad; s.camera.frame = &bad; g.go(s); CHECK(g.out.px == ref.px);
}

TEST(camera_split_screen_fit_and_crop) {
  Frame cam = stripeFrame(64, 48); Rig g;
  RenderState base = sceneState(7, 1, 700 * kNsPerMs, feats(0.4f, 0.4f, 0.4f, 0, false));
  g.go(base); Frame ref = g.out;
  RenderState s = base; s.camera.frame = &cam; s.camera.routed = true; s.camera.t.split = true;
  s.camera.t.fit = FitMode::Crop; g.go(s);
  for (int y = 0; y < 240; ++y) for (int x = 0; x < 160; ++x) if (g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]) { CHECK(false); y = 240; break; }
  CHECK(px(g.out, 200, 100) == 0x00FF00 && px(g.out, 280, 100) == 0x0000FF);     // centre crop of the 4:3 source into a 2:3 half
  s.camera.t.fit = FitMode::Fit; g.go(s);                                          // letterboxed: scene visible above/below
  CHECK(g.out.px[(size_t)10 * 320 + 200] == ref.px[(size_t)10 * 320 + 200]);
  CHECK(px(g.out, 170, 120) == 0xFF0000 && px(g.out, 310, 120) == 0xFFFFFF);
  s.camera.t.splitCameraRight = false; g.go(s);
  CHECK(px(g.out, 10, 120) == 0xFF0000 && g.out.px[(size_t)120 * 320 + 250] == ref.px[(size_t)120 * 320 + 250]);
}

TEST(camera_trails_bounded_and_shed_first) {
  Frame white = solidFrame(32, 24, 0xFFFFFF), black = solidFrame(32, 24, 0); Rig g;
  RenderState s = camState(&white); s.camera.t.trailFrames = 4;
  s.elapsedNs = 0; g.go(s); CHECK(px(g.out, 100, 100) == 0xFFFFFF);
  s.camera.frame = &black; s.elapsedNs = 40 * kNsPerMs; g.go(s);
  CHECK(near(ch(px(g.out, 100, 100), 0), 191, 1));                                 // 255 * (1-1/4)
  s.elapsedNs = 80 * kNsPerMs; g.go(s);
  CHECK(near(ch(px(g.out, 100, 100), 0), 143, 2));
  // decay depends on elapsed time, not on how many frames were rendered: one 80 ms step == two 40 ms steps
  Rig g2; s.camera.frame = &white; s.elapsedNs = 0; g2.go(s); s.camera.frame = &black; s.elapsedNs = 80 * kNsPerMs; g2.go(s);
  CHECK(near(ch(px(g2.out, 100, 100), 0), 143, 2));
  // longer trail (8) decays slower than short trail (2)
  auto after1 = [&](int n) { Rig q; RenderState t = camState(&white); t.camera.t.trailFrames = n; q.go(t); t.camera.frame = &black; t.elapsedNs = 40 * kNsPerMs; q.go(t); return ch(px(q.out, 5, 5), 0); };
  CHECK(after1(8) > after1(4) && after1(4) > after1(2) && after1(2) == 127);
  CHECK(near(after1(100), after1(8), 0));                                            // bounded at 8
  CHECK(after1(1) == 0 && after1(0) == 0);                                          // off below 2
  // shed: trailFramesCap 0 kills trails immediately
  Rig g3; RenderState t = camState(&white); t.camera.t.trailFrames = 8; t.shed = ShedKnobs::forLevel(1);
  g3.go(t); t.camera.frame = &black; t.elapsedNs = 40 * kNsPerMs; g3.go(t); CHECK(px(g3.out, 100, 100) == 0);
  CHECK(!g3.r.stats().trailsActive);
  // trails confined to the camera rect: stale trail does not leak outside PIP
  Rig g4; RenderState u = camState(&white); u.camera.t.trailFrames = 8; u.camera.t.pip = true; u.camera.t.pipRect = NormRect{0.0f, 0.0f, 0.5f, 0.5f};
  g4.go(u); u.camera.frame = &black; u.elapsedNs = 40 * kNsPerMs; g4.go(u);
  CHECK(px(g4.out, 300, 200) == 0 && ch(px(g4.out, 10, 10), 0) > 150);
}

TEST(media_layer_fit_crop_stretch_opacity) {
  Frame red = solidFrame(100, 50, 0xFF0000); Rig g;
  RenderState s = blackState(); s.media.frame = &red;
  s.media.fit = FitMode::Fit; g.go(s);
  CHECK(px(g.out, 160, 10) == 0 && px(g.out, 160, 120) == 0xFF0000 && px(g.out, 160, 230) == 0 && px(g.out, 2, 120) == 0xFF0000);
  s.media.fit = FitMode::Crop; g.go(s); CHECK(px(g.out, 2, 2) == 0xFF0000 && px(g.out, 317, 237) == 0xFF0000);
  s.media.fit = FitMode::Stretch; g.go(s); CHECK(px(g.out, 2, 2) == 0xFF0000 && px(g.out, 317, 237) == 0xFF0000);
  Frame lr; lr.resize(100, 50); for (int y = 0; y < 50; ++y) for (int x = 0; x < 100; ++x) lr.px[(size_t)y * 100 + x] = x < 50 ? 0xFF0000 : 0x0000FF;
  s.media.frame = &lr; s.media.crop = NormRect{0.5f, 0, 0.5f, 1}; g.go(s); CHECK(px(g.out, 5, 5) == 0x0000FF && px(g.out, 300, 200) == 0x0000FF);
  s.media.crop = NormRect(); s.media.opacity = 0.5f; s.media.fit = FitMode::Stretch; g.go(s);
  CHECK(nearRgb(px(g.out, 5, 5), 0x800000, 1) && nearRgb(px(g.out, 315, 5), 0x000080, 1));
  // media covering the frame is the same picture as media over black (procedural layer skipped, not drawn under)
  RenderState a = sceneState(8, 1, 500 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false)); a.media.frame = &red; a.media.fit = FitMode::Stretch;
  g.go(a); Frame o1 = g.out; RenderState b = blackState(); b.media.frame = &red; b.media.fit = FitMode::Stretch; g.go(b); CHECK(o1.px == g.out.px);
  // letterboxed media shows the procedural layer behind it
  a.media.fit = FitMode::Fit; g.go(a); CHECK(px(g.out, 160, 10) != 0);
}

TEST(camera_treatments_do_not_touch_media_or_scene) {
  Frame red = solidFrame(100, 75, 0xC03020), cam = stripeFrame(64, 48); Rig g;
  RenderState base = sceneState(5, 3, 900 * kNsPerMs, feats(0.5f, 0.2f, 0.9f, 0, false)); base.media.frame = &red; base.media.fit = FitMode::Fit;
  base.media.crop = NormRect{0.1f, 0.1f, 0.8f, 0.8f};
  g.go(base); Frame ref = g.out;
  RenderState s = base; s.camera.frame = &cam; s.camera.routed = false;
  s.camera.t.tintAmount = 1; s.camera.t.tintRgb = 0x00FF00; s.camera.t.threshold = true; s.camera.t.posterize = true; s.camera.t.mirror = true; s.camera.t.trailFrames = 8;
  g.go(s); CHECK(g.out.px == ref.px);
  // routed with a PIP only changes pixels inside the PIP rect even over media
  s.camera.routed = true; s.camera.t.pip = true; s.camera.t.pipRect = NormRect{0.7f, 0.0f, 0.3f, 0.3f}; g.go(s);
  bool leak = false; for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) { bool in = x >= 223 && x < 320 && y < 72; if (!in && g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]) leak = true; }
  CHECK(!leak);
}

TEST(composition_order_camera_over_media_text_over_camera) {
  Frame red = solidFrame(64, 48, 0xFF0000), green = solidFrame(64, 48, 0x00FF00); Rig g;
  RenderState s = blackState(); s.media.frame = &red; s.media.fit = FitMode::Stretch;
  s.camera.frame = &green; s.camera.routed = true; s.camera.t.fit = FitMode::Stretch;
  s.texts.resize(1); s.texts[0].text = U"MMMM"; s.texts[0].color = 0xFFFFFF; s.texts[0].shadow = false; s.texts[0].box = NormRect{0, 0, 1, 1};
  s.texts[0].v = VAlign::Middle; s.texts[0].h = HAlign::Center; s.texts[0].sizeFrac = 0.5f;
  g.go(s);
  int white = 0, greenpx = 0, redpx = 0;
  for (uint32_t p : g.out.px) { white += p == 0xFFFFFF; greenpx += p == 0x00FF00; redpx += p == 0xFF0000; }
  CHECK(white > 500 && greenpx > 10000 && redpx == 0);           // camera hides media; text drawn above camera
}

int main() { return ss_test::runAll(); }
