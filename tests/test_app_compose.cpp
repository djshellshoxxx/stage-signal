// RenderState composition from ShowSnapshot (pure): transitions, per-output overlays, camera routing, independent scenes, idle, keys.
#include "app_fakes.h"
#include "core/app/render_compose.h"
using namespace apptest;

struct Fix {
  show::ShowEngine e; show::ShowDocument doc; FakeClock clk; FeatureSnapshot f; uint64_t seq = 0; SceneClocks clocks;
  explicit Fix(show::ShowDocument d) : doc(std::move(d)) { e.replaceDocument(doc); f.silent = false; tick(10); }
  void tick(int ms) { for (int t = 0; t < ms; t += 10) { clk.advance(10 * kNsPerMs); f.sequence = ++seq; e.update(clk.t, f); } }
  ShowView view() { ShowView v; v.doc = std::make_shared<show::ShowDocument>(e.documentWithState()); v.snap = e.snapshot(); v.features = e.effectiveFeatures(); return v; }
  Action act(ActionId id, double val = 0, int target = -1) { Action a; a.id = id; a.value = val; a.target = target; return a; }
};

TEST(compose_utf8_decoding) {
  std::u32string s = utf8ToU32("Aé€😀");
  CHECK(s.size() == 4); CHECK(s[0] == U'A'); CHECK(s[1] == 0xE9); CHECK(s[2] == 0x20AC); CHECK(s[3] == 0x1F600);
  std::u32string bad = utf8ToU32(std::string("a\xC0\xAF" "b"));      // overlong sequence
  CHECK(bad.front() == U'a'); CHECK(bad.back() == U'b'); bool rep = false; for (auto c : bad) if (c == 0xFFFD) rep = true; CHECK(rep);
  CHECK(utf8ToU32(std::string(10000, 'x')).size() == 4096);
}

TEST(compose_steady_state_maps_scene_params_and_output_levels) {
  show::ShowDocument d = testDoc(); d.scenes[0].params["bars"] = 40.0f;
  Fix x(d);
  x.e.submit(x.act(ActionId::Brightness, 0.4), x.clk.t); x.tick(10);
  ShowView v = x.view(); ComposeExtras ex;
  RenderState s = composeState(v, 0, x.clocks, ex);
  CHECK(s.transition == TransitionType::None); CHECK(s.a.scene == sceneIndexById("spectrum_bars")); CHECK(!s.blackout);
  CHECK_NEAR(s.a.params.brightness, 0.4, 1e-6);
  CHECK_NEAR(s.a.params.extra[0], 40.0, 1e-6);               // scene param "bars" -> registry extra slot 0
}

TEST(compose_crossfade_and_cut_and_dip) {
  show::ShowDocument d = testDoc(); d.autoCfg.transition = {"fade", 1000};
  Fix x(d);
  x.e.submit(x.act(ActionId::SelectScene, 2), x.clk.t); x.tick(300);
  ShowView v = x.view(); ComposeExtras ex; RenderState s = composeState(v, 0, x.clocks, ex);
  CHECK(v.snap.transitionProgress < 1.0f);
  CHECK(s.transition == TransitionType::Crossfade); CHECK(s.a.scene == sceneIndexById("spectrum_bars")); CHECK(s.b.scene == sceneIndexById("tunnel"));
  CHECK(s.transitionProgress > 0 && s.transitionProgress < 1);
  x.tick(1000); v = x.view(); SceneClocks c2; s = composeState(v, 0, c2, ex);
  CHECK(s.transition == TransitionType::None); CHECK(s.a.scene == sceneIndexById("tunnel"));
  ShowView dv = v; dv.snap.outgoingSceneIndex = 0; dv.snap.transitionProgress = 0.25f; dv.snap.transitionType = "dip_black";
  s = composeState(dv, 0, c2, ex); CHECK(s.transition == TransitionType::None); CHECK_NEAR(s.masterBrightness, 0.5, 1e-6);
  dv.snap.transitionProgress = 0.75f; s = composeState(dv, 0, c2, ex); CHECK(s.transition == TransitionType::Cut); CHECK_NEAR(s.masterBrightness, 0.5, 1e-6);
  dv.snap.transitionType = "cut"; s = composeState(dv, 0, c2, ex); CHECK(s.transition == TransitionType::Cut);
}

TEST(compose_independent_output_has_own_scene_and_no_transition) {
  Fix x(testDoc());
  x.e.submit(x.act(ActionId::SetTarget, 1), x.clk.t);
  x.e.submit(x.act(ActionId::SelectScene, 3, 1), x.clk.t); x.tick(20);
  ShowView v = x.view(); ComposeExtras ex;
  SceneClocks c0, c1;
  RenderState s0 = composeState(v, 0, c0, ex), s1 = composeState(v, 1, c1, ex);
  CHECK(v.snap.outputs[1].independentScene);
  CHECK(s0.a.scene != s1.a.scene);
  CHECK(s1.a.scene == sceneIndexById("color_field")); CHECK(s0.a.scene == sceneIndexById("spectrum_bars"));
  CHECK(s1.transition == TransitionType::None);
  CHECK(compositionKey(s0, 100, 100) != compositionKey(s1, 100, 100));
}

TEST(compose_overlay_policy_per_output_and_published_text_only) {
  show::ShowDocument d = testDoc(); d.texts = {{"m1", "Hello"}}; d.outputs[0].overlay.tempo = true; d.outputs[1].overlay.text = false; d.manualBpm = 120;
  Fix x(d);
  ShowView v = x.view(); ComposeExtras ex; SceneClocks c;
  CHECK(composeState(v, 0, c, ex).texts.empty());                                 // nothing published yet: drafting never reaches outputs
  x.e.setDraft("Draft only"); x.e.previewDraft(); x.tick(10); v = x.view();
  CHECK(composeState(v, 0, c, ex).texts.empty());
  x.e.submit(x.act(ActionId::PublishText, 0), x.clk.t); x.tick(10); v = x.view();
  RenderState s0 = composeState(v, 0, c, ex), s1 = composeState(v, 1, c, ex);
  CHECK(s0.texts.size() == 1 && s0.texts[0].text == U"Hello");
  CHECK(s1.texts.empty());                                                         // output 1 has text overlay disabled
  CHECK(s0.overlays.showTempo); CHECK(!s1.overlays.showTempo); CHECK(s0.features.tempo.source == Estimate::Manual);
  ex.allowOverlays = false; CHECK(composeState(v, 0, c, ex).texts.empty());
}

TEST(compose_camera_is_routed_only_when_published) {
  Fix x(testDoc()); ShowView v = x.view(); SceneClocks c; Frame cam; cam.resize(4, 4);
  ComposeExtras ex; ex.camera = &cam; ex.cameraRouted = false;
  RenderState s = composeState(v, 0, c, ex); CHECK(!s.camera.routed); CHECK(s.camera.frame == nullptr);
  ex.cameraRouted = true; s = composeState(v, 0, c, ex); CHECK(s.camera.routed); CHECK(s.camera.frame == &cam);
  ex.camera = nullptr; s = composeState(v, 0, c, ex); CHECK(!s.camera.routed);        // lost/hidden camera: layer absent
}

TEST(compose_blackout_idle_and_unrenderable_scene_render_black) {
  Fix x(testDoc()); ShowView v = x.view(); SceneClocks c; ComposeExtras ex;
  CHECK(!composeState(v, 0, c, ex).blackout);
  x.e.submit(x.act(ActionId::VisualBlackout), x.clk.t); x.tick(10); v = x.view();
  CHECK(composeState(v, 0, c, ex).blackout);
  x.e.submit(x.act(ActionId::RestoreBlackout, 0), x.clk.t); x.tick(10); v = x.view();
  CHECK(!composeState(v, 0, c, ex).blackout);
  ShowView iv = v; iv.snap.idleActive = true; iv.snap.idleScreen = "black"; CHECK(composeState(iv, 0, c, ex).blackout);
  iv.snap.idleScreen = "color_field"; RenderState cs = composeState(iv, 0, c, ex); CHECK(!cs.blackout); CHECK(cs.a.scene == sceneIndexById("color_field"));
  ShowView nv = v; nv.snap.sceneIndex = -1; for (auto& o : nv.snap.outputs) o.sceneIndex = -1; CHECK(composeState(nv, 0, c, ex).blackout);
  ex.forceBlackout = true; CHECK(composeState(v, 0, c, ex).blackout);
}

TEST(compose_scene_clock_is_elapsed_time_based) {
  Fix x(testDoc()); ShowView v = x.view(); SceneClocks c; ComposeExtras ex;
  ex.elapsedNs = 5 * kNsPerSec; RenderState a = composeState(v, 0, c, ex);
  ex.elapsedNs = 5 * kNsPerSec + 2 * kNsPerSec;                       // frames skipped: time still advances by wall time
  RenderState b = composeState(v, 0, c, ex);
  CHECK(a.a.timeNs == 0); CHECK(b.a.timeNs == 2 * kNsPerSec);
}

TEST(compose_fit_scale_letterboxes) {
  Frame in; in.resize(40, 20); for (auto& p : in.px) p = 0xFF0000;
  Frame out; fitScaleFrame(in, 20, 20, out);
  CHECK(out.w == 20 && out.h == 20);
  CHECK(out.px[0] == 0);                         // top bar is black
  CHECK(out.px[10 * 20 + 10] == 0xFF0000);      // centre is content
}
int main() { return ss_test::runAll(); }
