// MOD-06 core renderer tests: registry, goldens, determinism, blackout, strobe, aspect fit, memory, params, shed.
#include "render_test_util.h"
#include <fstream>
#include <sstream>
#include <cmath>

using namespace rt;

static const char* kIds[12] = {"spectrum_bars", "waveform_ribbon", "geometric_orbit", "radial_pulse", "particle_field", "grid_pulse",
                               "tunnel", "color_field", "plasma", "starburst", "scope_lissajous", "ripple_rings"};

TEST(registry_has_exactly_the_twelve_scenes) {
  CHECK(sceneCount() == 12);
  for (int i = 0; i < 12; ++i) {
    CHECK(std::string(sceneInfo(i).id) == kIds[i]);
    CHECK(sceneIndexById(kIds[i]) == i);
    const SceneInfo& s = sceneInfo(i);
    CHECK(s.name && s.name[0]);
    CHECK(s.nParams == 8);
    CHECK(std::string(s.params[0].key) == "brightness" && std::string(s.params[1].key) == "speed");
    CHECK(std::string(s.params[2].key) == "intensity" && std::string(s.params[3].key) == "hue");
    for (int k = 0; k < s.nParams; ++k) CHECK(s.params[k].min < s.params[k].max && s.params[k].def >= s.params[k].min && s.params[k].def <= s.params[k].max);
    CHECK(s.fallback >= 0 && s.fallback < 12);
    CHECK(sceneInfo(s.fallback).cost <= s.cost);                   // fallback never costlier
    SceneParams d = defaultParams(i);
    SceneParams c = clampParams(i, d);
    CHECK(c.extra[0] == d.extra[0] && c.extra[3] == d.extra[3]);
  }
  CHECK(sceneIndexById("nope") == -1);
  int strobe = 0; for (int i = 0; i < 12; ++i) strobe += sceneInfo(i).strobe;
  CHECK(strobe >= 2);
  SceneParams wild; wild.brightness = 9; wild.speed = -3; wild.intensity = 5; wild.hue = -1; wild.extra[0] = 1e6f;
  SceneParams c = clampParams(0, wild);
  CHECK(c.brightness == 1.0f && c.speed == 0.25f && c.intensity == 1.0f && c.hue == 0.0f && c.extra[0] == 64.0f);
  CHECK(c.extra[1] == sceneInfo(0).params[5].def);                 // kParamDefault resolves to registry default
}

// ---- goldens ---------------------------------------------------------------------------------------------------
static std::string goldenPath() {
  const char* e = std::getenv("SS_GOLDEN_PATH"); if (e && *e) return e;
  const char* cands[] = {"tests/fixtures/golden_render.txt", "../tests/fixtures/golden_render.txt", "../../tests/fixtures/golden_render.txt"};
  for (const char* c : cands) { std::ifstream f(c); if (f) return c; }
  return cands[0];
}

TEST(golden_frames_match) {
  std::map<std::string, uint64_t> got;
  Renderer r; CHECK(r.configure(320, 240, 1.0f).ok());
  Frame out;
  for (int i = 0; i < 12; ++i) {
    RenderState a = sceneState(i, 7, 1500 * kNsPerMs, feats(0.8f, 0.5f, 0.25f, 0.5f, false));
    r.render(out, a); got[std::string(kIds[i]) + "/seed7_t1500"] = fnv(out);
    RenderState b = sceneState(i, 4242, 3120 * kNsPerMs, feats(0.25f, 0.75f, 0.5f, 1.0f, true, -6.0f));
    b.a.params.hue = 0.25f; b.a.params.speed = 2.0f;
    r.render(out, b); got[std::string(kIds[i]) + "/seed4242_t3120_onset"] = fnv(out);
    RenderState c = sceneState(i, 1, 0, FeatureSnapshot());
    r.render(out, c); got[std::string(kIds[i]) + "/silent_t0"] = fnv(out);
  }
  {   // composite goldens: crossfade, half-res crossfade, text + overlays, camera treatments
    RenderState s = sceneState(0, 5, 2000 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
    s.b = s.a; s.b.scene = 8; s.b.params = defaultParams(8); s.transition = TransitionType::Crossfade; s.transitionProgress = 0.5f;
    r.render(out, s); got["composite/crossfade_bars_plasma_50"] = fnv(out);
    s.shed.halfResTransition = true; r.render(out, s); got["composite/crossfade_halfres"] = fnv(out);
    RenderState t = sceneState(7, 5, 1000 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
    t.texts.resize(1); t.texts[0].text = U"Hello, StageSignal! Été naïve façade\nSecond line wraps around when it is long enough to need it.";
    t.texts[0].alpha = 0.75f; t.overlays.showTempo = true; t.overlays.showKey = true;
    t.features.tempo.valid = true; t.features.tempo.value = 128; t.features.tempo.confidence = 0.5f;
    r.render(out, t); got["composite/text_overlays"] = fnv(out);
    Frame cam = stripeFrame(64, 48);
    RenderState c = sceneState(7, 5, 1000 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
    c.camera.frame = &cam; c.camera.routed = true; c.camera.t.pip = true; c.camera.t.mirror = true; c.camera.t.tintAmount = 0.25f; c.camera.t.posterize = true;
    r.render(out, c); got["composite/camera_pip_mirror_tint_poster"] = fnv(out);
  }
  const std::string path = goldenPath();
  const char* upd = std::getenv("SS_UPDATE_GOLDENS");
  if (upd && *upd == '1') {
    std::ofstream f(path.c_str());
    f << "# MOD-06 golden frame FNV-1a64 checksums (320x240, scale 1.0). Regenerate: SS_UPDATE_GOLDENS=1 ./test_render_core\n";
    for (auto& kv : got) f << kv.first << " " << hex64(kv.second) << "\n";
    std::printf("  wrote %zu goldens to %s\n", got.size(), path.c_str());
    return;
  }
  std::ifstream f(path.c_str());
  CHECK((bool)f);
  std::map<std::string, std::string> want; std::string line;
  while (std::getline(f, line)) { if (line.empty() || line[0] == '#') continue; std::istringstream is(line); std::string k, v; is >> k >> v; want[k] = v; }
  CHECK(want.size() == got.size());
  for (auto& kv : got) {
    auto it = want.find(kv.first);
    if (it == want.end()) { std::printf("  missing golden %s\n", kv.first.c_str()); CHECK(false); continue; }
    if (it->second != hex64(kv.second)) { std::printf("  golden mismatch %s want %s got %s\n", kv.first.c_str(), it->second.c_str(), hex64(kv.second).c_str()); CHECK(false); }
  }
}

TEST(scenes_are_not_blank_and_distinct) {
  Renderer r; r.configure(320, 240, 1.0f); Frame out; std::map<uint64_t, int> seen;
  for (int i = 0; i < 12; ++i) {
    r.render(out, sceneState(i, 7, 1500 * kNsPerMs, feats(0.8f, 0.5f, 0.25f, 0.5f, false)));
    if (!(meanLuma(out) > 1.0)) std::printf("  dim scene %s luma %.2f\n", sceneInfo(i).id, meanLuma(out));
    CHECK(meanLuma(out) > 1.0);
    seen[fnv(out)]++;
  }
  CHECK(seen.size() == 12);
}

TEST(deterministic_across_renderer_instances_and_seed_matters) {
  Frame a, b, c; Renderer r1, r2; r1.configure(320, 240, 1.0f); r2.configure(320, 240, 1.0f);
  for (int i = 0; i < 12; ++i) {
    RenderState s = sceneState(i, 99, 777 * kNsPerMs, feats(0.6f, 0.3f, 0.9f, 0.2f, false));
    r1.render(a, s); r2.render(b, s);
    CHECK(a.px == b.px);
    s.a.seed = 100; r1.render(c, s);
    if (i == 4 || i == 0 || i == 11 || i == 9) { if (a.px == c.px) std::printf("  seed no effect %s\n", sceneInfo(i).id); CHECK(a.px != c.px); }   // seeded scenes
  }
}

TEST(frame_skip_invariance) {
  // The picture at time T must not depend on which earlier frames were rendered (animation derives from elapsed time).
  for (int i = 0; i < 12; ++i) {
    Renderer full, skip; full.configure(320, 240, 1.0f); skip.configure(320, 240, 1.0f);
    Frame a, b; FeatureSnapshot f = feats(0.7f, 0.4f, 0.2f, 0.3f, false);
    for (int k = 0; k <= 50; ++k) full.render(a, sceneState(i, 3, (Ns)k * 40 * kNsPerMs, f));
    skip.render(b, sceneState(i, 3, 50LL * 40 * kNsPerMs, f));
    CHECK(a.px == b.px);
  }
  // speed multiplies elapsed time: speed 2 at t == speed 1 at 2t
  for (int i = 0; i < 12; ++i) {
    Renderer r; r.configure(160, 120, 1.0f); Frame a, b; FeatureSnapshot f = feats(0.7f, 0.4f, 0.2f, 0.3f, false);
    RenderState s1 = sceneState(i, 3, 500 * kNsPerMs, f); s1.a.params.speed = 2.0f;
    RenderState s2 = sceneState(i, 3, 1000 * kNsPerMs, f); s2.a.params.speed = 1.0f;
    r.render(a, s1); r.render(b, s2);
    CHECK(a.px == b.px);
  }
}

TEST(blackout_is_black_on_very_next_render) {
  Renderer r; r.configure(320, 240, 0.5f); Frame out, ref;
  RenderState s = sceneState(8, 1, 1000 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
  s.texts.resize(1); s.texts[0].text = U"VISIBLE"; s.refBorder = true; s.overlays.showTempo = true;
  Frame cam = solidFrame(32, 24, 0xFFFFFF); s.camera.frame = &cam; s.camera.routed = true; s.camera.t.trailFrames = 4;
  r.render(ref, s);
  CHECK(meanLuma(ref) > 5);
  s.blackout = true;
  r.render(out, s);
  bool allBlack = true; for (uint32_t p : out.px) if (p) { allBlack = false; break; }
  CHECK(allBlack);
  s.blackout = false; r.render(out, s);
  CHECK(out.px == ref.px);                                      // blackout is a pure mask: content resumes unchanged
  s.masterBrightness = 0.0f; r.render(out, s);
  allBlack = true; for (uint32_t p : out.px) if (p) { allBlack = false; break; }
  CHECK(allBlack);
}

TEST(master_and_scene_brightness) {
  Renderer r; r.configure(160, 120, 1.0f); Frame a, b;
  RenderState s = sceneState(7, 1, 0, FeatureSnapshot());
  r.render(a, s);
  s.masterBrightness = 0.5f; r.render(b, s);
  double ratio = meanLuma(b) / meanLuma(a);
  CHECK_NEAR(ratio, 0.5, 0.02);
  s.masterBrightness = 1.0f; s.a.params.brightness = 0.0f; r.render(b, s);
  CHECK(meanLuma(b) == 0.0);
}

TEST(strobe_disable_limits_frame_to_frame_luminance_change) {
  const double kThresh = 0.10 * 255.0;                           // 10% of full scale per frame
  Renderer r; r.configure(160, 120, 1.0f); Frame out;
  int strobeScenes = 0, exceededWhenEnabled = 0;
  for (int i = 0; i < 12; ++i) {
    if (!sceneInfo(i).strobe) continue;
    ++strobeScenes;
    for (int mode = 0; mode < 2; ++mode) {
      bool disable = mode == 1; double prev = -1, worst = 0;
      for (int k = 0; k < 60; ++k) {                              // worst-case drive: onset + bands flip every frame
        bool hot = (k & 1) == 0;
        RenderState s = sceneState(i, 1, (Ns)k * 40 * kNsPerMs, hot ? feats(1, 1, 1, 1, true, -3) : feats(0, 0, 0, 0, false, -90));
        s.disableStrobing = disable;
        r.render(out, s);
        double l = meanLuma(out);
        if (prev >= 0) worst = std::max(worst, std::fabs(l - prev));
        prev = l;
      }
      if (disable) { std::printf("  %s strobe-off worst delta %.1f\n", sceneInfo(i).id, worst); CHECK(worst <= kThresh); }
      else if (worst > kThresh) ++exceededWhenEnabled;
    }
    // with strobing disabled and a typical track (onset flag every 5th frame) the guard must not be needed to stay calm either
    double prev = -1, worst = 0;
    for (int k = 0; k < 60; ++k) {
      RenderState s = sceneState(i, 1, (Ns)k * 40 * kNsPerMs, feats(0.6f, 0.4f, 0.3f, (k % 5 == 0) ? 1.0f : 0.2f, k % 5 == 0));
      s.disableStrobing = true; r.render(out, s);
      double l = meanLuma(out); if (prev >= 0) worst = std::max(worst, std::fabs(l - prev)); prev = l;
    }
    CHECK(worst <= kThresh);
  }
  CHECK(strobeScenes >= 2);
  CHECK(exceededWhenEnabled >= 2);                               // flagged scenes really do strobe when allowed
}

TEST(aspect_fit_4x3_16x9_portrait_and_reference_border) {
  struct Case { int w, h, an, ad; int ew, eh; } cs[] = {
    {800, 600, 4, 3, 800, 600}, {1280, 720, 4, 3, 960, 720}, {1920, 1080, 4, 3, 1440, 1080}, {600, 800, 16, 9, 600, 337},
    {600, 800, 3, 4, 600, 800}, {800, 600, 16, 9, 800, 450}, {800, 600, 0, 0, 800, 600}, {1280, 720, 16, 9, 1280, 720}};
  for (auto& c : cs) {
    IRect f = Renderer::fitRect(c.w, c.h, c.an, c.ad);
    CHECK(f.w == c.ew && f.h == c.eh);
    CHECK(f.x == (c.w - f.w) / 2 && f.y == (c.h - f.h) / 2);
  }
  // render with comp aspect 4:3 into 16:9 output (pillarbox) and check the border/markers (REQ-D04)
  Renderer r; CHECK(r.configure(640, 360, 0.5f).ok()); Frame out;
  RenderState s = sceneState(7, 1, 0, FeatureSnapshot()); s.compAspectNum = 4; s.compAspectDen = 3; s.refBorder = true;
  r.render(out, s);
  IRect fit = r.stats().fit; CHECK(fit.w == 480 && fit.h == 360 && fit.x == 80);
  CHECK(px(out, 10, 100) == 0 && px(out, fit.x - 1, 100) == 0 && px(out, fit.x + fit.w, 100) == 0);   // pillarbox black
  CHECK(px(out, fit.x, 180) == 0xFFFFFF && px(out, fit.x + fit.w - 1, 180) == 0xFFFFFF);
  CHECK(px(out, 320, 0) == 0xFFFFFF && px(out, 320, 359) == 0xFFFFFF);
  CHECK(px(out, fit.x + 1, 1) == 0xFF0000 && px(out, fit.x + fit.w - 2, 1) == 0x00FF00);
  CHECK(px(out, fit.x + 1, 358) == 0x0000FF && px(out, fit.x + fit.w - 2, 358) == 0xFFFF00);
  CHECK(px(out, 320, 180) != 0xFFFFFF);                           // interior not covered
  // internal buffer follows the fit rect aspect
  CHECK(r.stats().internalW * 3 == r.stats().internalH * 4);
  // 800x600 exactly fills its target and border is at the screen edge
  Renderer r2; r2.configure(800, 600, 0.5f); Frame o2; RenderState s2 = s; s2.compAspectNum = 0; r2.render(o2, s2);
  CHECK(r2.stats().fit.w == 800 && r2.stats().fit.h == 600);
  CHECK(px(o2, 0, 300) == 0xFFFFFF && px(o2, 799, 300) == 0xFFFFFF && px(o2, 400, 0) == 0xFFFFFF && px(o2, 400, 599) == 0xFFFFFF);
}

TEST(rotation_and_resolution_changes_recompute_normalized_composition) {
  // color_field corner colors are resolution/orientation independent
  Renderer r; Frame a, b, c;
  RenderState s = sceneState(7, 1, 500 * kNsPerMs, feats(0.2f, 0.2f, 0.2f, 0, false));
  r.configure(320, 240, 1.0f); r.render(a, s);
  r.configure(240, 320, 1.0f); r.render(b, s);                    // portrait / rotated 90
  r.configure(640, 360, 0.5f); r.render(c, s);                    // 16:9, reduced internal scale
  auto near = [](int p, int q) { for (int sh = 0; sh <= 16; sh += 8) if (std::abs(ch(p, sh) - ch(q, sh)) > 6) return false; return true; };
  CHECK(near(px(a, 0, 0), px(b, 0, 0)) && near(px(a, 319, 0), px(b, 239, 0)));
  CHECK(near(px(a, 0, 239), px(b, 0, 319)) && near(px(a, 319, 239), px(b, 239, 319)));
  CHECK(near(px(a, 0, 0), px(c, 0, 0)) && near(px(a, 319, 239), px(c, 639, 359)));
  // spectrum bars occupy the same normalized horizontal span on all aspects (first bar starts at left edge, last ends at right)
  for (auto wh : {std::make_pair(320, 240), std::make_pair(240, 320), std::make_pair(640, 360), std::make_pair(360, 640)}) {
    r.configure(wh.first, wh.second, 1.0f); Frame f; r.render(f, sceneState(0, 1, 0, feats(1, 1, 1, 0, false)));
    int lit0 = 0, lit1 = 0;
    for (int y = 0; y < f.h; ++y) for (int x = 0; x < f.w / 12; ++x) { lit0 += px(f, x, y) != 0; lit1 += px(f, f.w - 1 - x, y) != 0; }
    CHECK(lit0 > 0 && lit1 > 0);
  }
}

TEST(memory_budgets) {
  Renderer r; CHECK(r.configure(800, 600, 1.0f).ok());
  std::printf("  pools @800x600 scale1.0: %.2f MiB\n", r.poolBytes() / 1048576.0);
  CHECK(r.poolBytes() <= Renderer::kPoolBudgetBytes);
  Renderer r2; CHECK(r2.configure(800, 600, 0.5f).ok());
  std::printf("  pools @800x600 scale0.5: %.2f MiB\n", r2.poolBytes() / 1048576.0);
  CHECK(r2.poolBytes() < r.poolBytes());
  Renderer big; CHECK(big.configure(4096, 4096, 1.0f).code == Err::ResourceExhausted);
  CHECK(big.configure(4, 4, 1.0f).code == Err::InvalidArgument);
  // after many renders pool size is unchanged (no growth), including transitions/half-res/camera trails
  Frame out, cam = solidFrame(64, 48, 0x808080);
  size_t before = r.poolBytes();
  for (int k = 0; k < 40; ++k) {
    RenderState s = sceneState(k % 12, 1, (Ns)k * 40 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0.5f, false));
    s.b = s.a; s.b.scene = (k * 5 + 3) % 12; s.b.params = defaultParams(s.b.scene); s.transition = TransitionType::Crossfade; s.transitionProgress = 0.3f;
    s.shed.halfResTransition = (k & 1); s.internalScale = (k % 3 == 0) ? 0.5f : 0.0f;
    s.camera.frame = &cam; s.camera.routed = true; s.camera.t.trailFrames = 6;
    r.render(out, s);
  }
  CHECK(r.poolBytes() == before);
}

TEST(scene_params_and_shed) {
  Renderer r; r.configure(320, 240, 1.0f); Frame a, b;
  RenderState s = sceneState(1, 1, 400 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
  r.render(a, s); s.a.params.hue = 0.5f; r.render(b, s); CHECK(a.px != b.px);
  s = sceneState(4, 1, 400 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
  auto lit = [](const Frame& f) { int n = 0; for (uint32_t p : f.px) n += p != 0; return n; };
  r.render(a, s); s.shed.particlePercent = 25; r.render(b, s); CHECK(lit(b) < lit(a) * 6 / 10);
  s = sceneState(8, 1, 400 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
  s.shed = ShedKnobs::forLevel(3); r.render(a, s);
  CHECK(r.stats().sceneA == 7);                                   // plasma -> color_field fallback
  s = sceneState(0, 1, 400 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false)); s.shed = ShedKnobs::forLevel(3); r.render(a, s);
  CHECK(r.stats().sceneA == 0);                                   // Low cost scenes keep themselves
  CHECK(ShedKnobs::forLevel(1).trailFramesCap == 0 && ShedKnobs::forLevel(1).particlePercent == 100);   // trails shed first
  // internal render scale lowers the buffer, not the output size
  s.internalScale = 0.25f; r.render(a, s); CHECK(a.w == 320 && r.stats().internalW == 80 && r.stats().internalH == 60);
  // crossfade endpoints equal the single scenes; Cut shows B
  RenderState x = sceneState(0, 1, 400 * kNsPerMs, feats(0.5f, 0.5f, 0.5f, 0, false));
  x.b = x.a; x.b.scene = 7; x.b.params = defaultParams(7);
  Frame fa, fb, f0, f1, fc;
  RenderState onlyA = x; onlyA.transition = TransitionType::None; r.render(fa, onlyA);
  RenderState onlyB = x; onlyB.transition = TransitionType::Cut; r.render(fb, onlyB);
  x.transition = TransitionType::Crossfade; x.transitionProgress = 0.0f; r.render(f0, x);
  x.transitionProgress = 1.0f; r.render(f1, x);
  x.transitionProgress = 0.5f; r.render(fc, x);
  CHECK(f0.px == fa.px && f1.px == fb.px && fc.px != fa.px && fc.px != fb.px);
  double mid = meanLuma(fc), la = meanLuma(fa), lb = meanLuma(fb);
  CHECK_NEAR(mid, (la + lb) / 2, 3.0);
}

int main() { return ss_test::runAll(); }
