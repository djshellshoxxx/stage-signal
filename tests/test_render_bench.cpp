// MOD-06 micro-benchmark: ms/frame per scene at 800x600. Budget (spec §7): 25 FPS => 40 ms period, p95 <= 80% = 32 ms on the
// 800 MHz BASE core; this machine is ~4x faster, so the pass line here is p95 < 6 ms at the default internal scale (0.5).
// SS_BENCH_BUDGET_MS overrides the CHECK limit (default 12 ms = 2x slack for noisy CI); SS_BENCH_STRICT=1 enforces 6 ms.
#include "render_test_util.h"
#include <algorithm>
#include <chrono>

using namespace rt;

namespace {
double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Stat { double p50, p95, max; };
Stat measure(Renderer& r, Frame& out, const std::function<RenderState(int)>& mk, int frames) {
  for (int i = 0; i < 8; ++i) r.render(out, mk(i));        // warm caches / lazy maps
  std::vector<double> v; v.reserve(frames);
  for (int i = 0; i < frames; ++i) { RenderState s = mk(8 + i); double t0 = nowMs(); r.render(out, s); v.push_back(nowMs() - t0); }
  std::sort(v.begin(), v.end());
  return {v[v.size() / 2], v[(size_t)(v.size() * 95 / 100)], v.back()};
}
RenderState animated(int scene, int k) {
  Ns t = (Ns)k * 40 * kNsPerMs;
  float ph = (float)(k % 25) / 25.0f;
  RenderState s = sceneState(scene, 7, t, feats(0.3f + 0.6f * ph, 0.5f, 0.2f + 0.5f * (1 - ph), (k % 12 == 0) ? 1.0f : 0.3f, k % 12 == 0));
  return s;
}
}

TEST(bench_scenes_800x600) {
  double limit = 12.0;
  if (const char* e = std::getenv("SS_BENCH_BUDGET_MS")) limit = std::atof(e);
  const char* strict = std::getenv("SS_BENCH_STRICT"); if (strict && *strict == '1') limit = 6.0;
  std::printf("  %-18s %-5s | scale 1.0 p50/p95/max ms | scale 0.5 p50/p95/max ms\n", "scene", "cost");
  double worst95 = 0;
  for (int i = 0; i < 12; ++i) {
    Stat st[2];
    for (int k = 0; k < 2; ++k) {
      Renderer r; CHECK(r.configure(800, 600, k == 0 ? 1.0f : 0.5f).ok()); Frame out;
      st[k] = measure(r, out, [&](int f) { return animated(i, f); }, 150);
    }
    std::printf("  %-18s %-5s | %6.2f %6.2f %6.2f       | %6.2f %6.2f %6.2f\n", sceneInfo(i).id, sceneInfo(i).cost == CostClass::Low ? "low" : (sceneInfo(i).cost == CostClass::Medium ? "med" : "high"),
                st[0].p50, st[0].p95, st[0].max, st[1].p50, st[1].p95, st[1].max);
    worst95 = std::max(worst95, st[1].p95);
    CHECK(st[1].p95 < limit);
  }
  std::printf("  worst p95 @ scale 0.5 = %.2f ms (limit %.1f)\n", worst95, limit);
}

TEST(bench_composite_basic_profile) {
  // BASE qualification workload: crossfade of two scenes + text + camera 320x240 with trails + overlays, at default scale 0.5.
  Renderer r; CHECK(r.configure(800, 600, 0.5f).ok()); Frame out, cam = solidFrame(320, 240, 0x406080);
  for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) cam.px[(size_t)y * 320 + x] = rgb(x * 255 / 320, y * 255 / 240, (x ^ y) & 255);
  Frame still = solidFrame(640, 480, 0x203040);
  auto mk = [&](int k) {
    RenderState s = animated(8, k); s.b = s.a; s.b.scene = 11; s.b.params = defaultParams(11); s.transition = TransitionType::Crossfade; s.transitionProgress = 0.5f;
    s.camera.frame = &cam; s.camera.routed = true; s.camera.t.pip = true; s.camera.t.mirror = true; s.camera.t.tintAmount = 0.3f; s.camera.t.trailFrames = 4;
    s.media.frame = &still; s.media.fit = FitMode::Fit; s.media.opacity = 0.5f;
    s.texts.resize(1); s.texts[0].text = U"StageSignal live: scrolling text benchmark line, with wrap."; s.texts[0].scrollPx = k;
    s.overlays.showTempo = true; s.overlays.showKey = true; s.features.tempo.valid = true; s.features.tempo.value = 126;
    return s;
  };
  Stat a = measure(r, out, mk, 200);
  std::printf("  composite (plasma x ripple crossfade + still + camera trails + text + overlays) @0.5: p50 %.2f p95 %.2f max %.2f ms\n", a.p50, a.p95, a.max);
  CHECK(a.p95 < 20.0);
  // shed ladder: level 3 (half-res crossfade, fallbacks, particles) should be cheaper
  auto mk3 = [&](int k) { RenderState s = mk(k); s.shed = ShedKnobs::forLevel(3); return s; };
  Stat b = measure(r, out, mk3, 200);
  std::printf("  same at shed level 3: p50 %.2f p95 %.2f max %.2f ms\n", b.p50, b.p95, b.max);
  CHECK(b.p50 <= a.p50 * 1.05);
}

int main() { return ss_test::runAll(); }
