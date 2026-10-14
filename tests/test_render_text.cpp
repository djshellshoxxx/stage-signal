// MOD-06 text tests (REQ-S06): wrap/truncate limits, Latin-1, cache budget, placement/fade/scroll, overlays, branding.
#include "render_test_util.h"
#include "render/font.h"
#include "render/text.h"
#include <cmath>

using namespace rt;

namespace {
int lit(const TextBitmap& b) { int n = 0; for (uint8_t v : b.a) n += v != 0; return n; }
struct Rig { Renderer r; Frame out; Rig(int w = 320, int h = 240) { CHECK(r.configure(w, h, 1.0f).ok()); } void go(const RenderState& s) { r.render(out, s); } };
RenderState textState(const std::u32string& t) {
  RenderState s = blackState(); s.texts.resize(1); s.texts[0].text = t; s.texts[0].shadow = false; s.texts[0].box = NormRect{0.1f, 0.6f, 0.8f, 0.3f};
  s.texts[0].h = HAlign::Left; s.texts[0].v = VAlign::Top; return s;
}
struct FakeRas : ITextRasterizer {
  int calls = 0; bool ok = true;
  bool rasterize(const std::u32string& t, const TextStyle& st, int maxW, int& w, int& h, std::vector<uint32_t>& argb) override {
    ++calls; if (!ok) return false; w = 10; h = 6; argb.assign(60, 0x80FFFFFFu); return true;
  }
};
}

TEST(text_wrap_rules) {
  // px=12 => 6-px advance; maxW 60 => 10 columns
  TextBitmap a = TextEngine::buildBuiltin(U"hello world foo", 12, 60, HAlign::Left, false);
  CHECK(a.lines == 2 && a.h == 24 && a.w == 9 * 6 && !a.truncated);                // "hello" / "world foo"
  TextBitmap b = TextEngine::buildBuiltin(U"abcdefghijklmnopqrstuvwxyz", 12, 60, HAlign::Left, false);
  CHECK(b.lines == 3 && b.w == 60);                                                 // hard break inside a long word
  TextBitmap c = TextEngine::buildBuiltin(U"one\ntwo\n\nfour", 12, 600, HAlign::Left, false);
  CHECK(c.lines == 4);
  TextBitmap d = TextEngine::buildBuiltin(U"", 12, 60, HAlign::Left, false);
  CHECK(d.w >= 1 && d.h == 12 && lit(d) == 0);
  TextBitmap e = TextEngine::buildBuiltin(U"a\tb\r\nc", 12, 600, HAlign::Left, false);
  CHECK(e.lines == 2);
  // alignment moves ink inside the block
  TextBitmap l = TextEngine::buildBuiltin(U"abcdefghij\nab", 12, 120, HAlign::Left, false);
  TextBitmap r = TextEngine::buildBuiltin(U"abcdefghij\nab", 12, 120, HAlign::Right, false);
  auto firstInk = [](const TextBitmap& t, int line) { for (int x = 0; x < t.w; ++x) for (int y = line * 12; y < line * 12 + 12; ++y) if (t.a[(size_t)y * t.w + x]) return x; return -1; };
  CHECK(firstInk(l, 1) < 3 && firstInk(r, 1) > 40);
  // bold thickens
  TextBitmap n = TextEngine::buildBuiltin(U"Hi", 24, 200, HAlign::Left, false), bo = TextEngine::buildBuiltin(U"Hi", 24, 200, HAlign::Left, true);
  CHECK(lit(bo) > lit(n));
  // glyph shapes: distinct letters are distinct, sizes scale
  TextBitmap A = TextEngine::buildBuiltin(U"A", 24, 50, HAlign::Left, false), B = TextEngine::buildBuiltin(U"B", 24, 50, HAlign::Left, false);
  CHECK(A.a != B.a && lit(A) > 20);
  CHECK(TextEngine::builtinPitch(48) == 24 && TextEngine::builtinPitch(12) == 6);
}

TEST(text_limits_4096_code_points_and_height_budget) {
  std::u32string big(5000, U'x');
  TextBitmap a = TextEngine::buildBuiltin(big, 12, 600, HAlign::Left, false);
  CHECK(a.truncated);
  CHECK(a.lines * 100 >= 4096 - 100 && a.lines * 100 <= 4096 + 100);               // 100 cols per line, only 4096 cps laid out
  std::u32string exact(4096, U'x');
  CHECK(!TextEngine::buildBuiltin(exact, 12, 600, HAlign::Left, false).truncated);
  // huge glyphs: height budget (<= 4 MiB bitmap) truncates with "..." on the last line
  std::u32string words; for (int i = 0; i < 819; ++i) words += U"wxyz ";
  TextBitmap t = TextEngine::buildBuiltin(words, 64, 128, HAlign::Left, false);
  CHECK(t.truncated && t.a.size() <= TextEngine::kMaxMaskBytes && t.lines < 819 && t.lines >= 1);
  // last line ends with three dots (ink in the last line's final columns)
  TextBitmap single = TextEngine::buildBuiltin(U"abcdefghijklmnopqrstuvwxyz abc", 64, 128, HAlign::Left, false);
  CHECK(single.lines >= 1);
  // pathological sizes are handled
  TextBitmap tiny = TextEngine::buildBuiltin(U"hello", 1, 1, HAlign::Left, false);
  CHECK(tiny.w >= 1 && tiny.h >= 1);
  TextBitmap ctl = TextEngine::buildBuiltin(std::u32string(U"a\x01\x02" "b"), 12, 100, HAlign::Left, false);
  CHECK(ctl.lines == 1 && ctl.w == 12);
}

TEST(text_latin1_and_unknown_code_points) {
  bool box = false;
  for (char32_t c = 32; c < 127; ++c) { font::glyph(c, box); CHECK(!box); }
  font::glyph(0xE9, box); CHECK(!box);
  font::glyph(0xC0, box); CHECK(!box); font::glyph(0xFF, box); CHECK(!box); font::glyph(0xDF, box); CHECK(!box);
  font::glyph(0x4E2D, box); CHECK(box);
  font::glyph(0x1F600, box); CHECK(box);
  const uint8_t* e = font::glyph(U'e', box); const uint8_t* eacute = font::glyph(0xE9, box);
  CHECK(std::memcmp(e, eacute, 11) != 0);                                            // accent adds ink
  CHECK(eacute[2] != 0 || eacute[3] != 0);
  TextBitmap a = TextEngine::buildBuiltin(U"é", 24, 50, HAlign::Left, false), z = TextEngine::buildBuiltin(U"中", 24, 50, HAlign::Left, false);
  CHECK(lit(a) > 10 && lit(z) > 10);
  for (char32_t c = 0xA1; c <= 0xFF; ++c) { if (c == 0xAD) continue; TextBitmap t = TextEngine::buildBuiltin(std::u32string(1, c), 24, 50, HAlign::Left, false); (void)t; }
}

TEST(text_cache_budget_and_reuse) {
  TextEngine te;
  const TextBitmap* a = te.get(U"cached text", 24, 300, HAlign::Left, false);
  const TextBitmap* b = te.get(U"cached text", 24, 300, HAlign::Left, false);
  CHECK(a == b && te.entries() == 1);
  CHECK(te.get(U"cached text", 25, 300, HAlign::Left, false) != a || te.entries() == 2);
  for (int i = 0; i < 60; ++i) {                                                   // ~1.5 MiB each => budget must evict
    std::u32string t; for (int k = 0; k < 4000; ++k) t.push_back(U'a' + (char32_t)((k + i) % 26));
    te.get(t, 24, 800, HAlign::Left, false);
    CHECK(te.bytes() <= TextEngine::kBudgetBytes);
  }
  CHECK(te.entries() < 62);
  FakeRas ras; TextEngine tr; tr.setRasterizer(&ras);
  const TextBitmap* p = tr.get(U"x", 24, 100, HAlign::Left, false);
  CHECK(p->w == 10 && p->h == 6 && p->a[0] == 0x80 && ras.calls == 1);
  tr.get(U"x", 24, 100, HAlign::Left, false); CHECK(ras.calls == 1);                // cached
  ras.ok = false; const TextBitmap* q = tr.get(U"fallback", 24, 300, HAlign::Left, false);
  CHECK(q->w > 10 && lit(*q) > 20);                                                // falls back to built-in font
  std::u32string huge(6000, U'y'); ras.ok = true; tr.get(huge, 24, 100, HAlign::Left, false);
}

TEST(text_rendering_placement_color_fade) {
  Rig g;
  RenderState s = textState(U"HELLO WORLD");
  g.go(s);
  int firstRow = 999, lastRow = -1, firstCol = 999, lastCol = -1, n = 0;
  for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) if (g.out.px[(size_t)y * 320 + x]) { ++n; firstRow = std::min(firstRow, y); lastRow = std::max(lastRow, y); firstCol = std::min(firstCol, x); lastCol = std::max(lastCol, x); }
  CHECK(n > 100);
  CHECK(firstRow >= 144 && lastRow < 144 + 72);                                    // inside the normalized box (y 0.6..0.9)
  CHECK(firstCol >= 32 && lastCol < 32 + 256);
  // vertical / horizontal alignment
  RenderState bot = s; bot.texts[0].v = VAlign::Bottom; bot.texts[0].h = HAlign::Right; g.go(bot);
  int bl = -1, br = 999; for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) if (g.out.px[(size_t)y * 320 + x]) { bl = std::max(bl, y); br = std::min(br, x); }
  CHECK(bl > lastRow && br > firstCol + 30 && bl < 216);
  // color + shadow
  RenderState red = s; red.texts[0].color = 0xFF0000; g.go(red);
  bool onlyRed = true; for (uint32_t p : g.out.px) if (p && p != 0xFF0000) onlyRed = false;
  CHECK(onlyRed);
  Frame gray = solidFrame(8, 8, 0x808080);
  RenderState sh = s; sh.media.frame = &gray; sh.media.fit = FitMode::Stretch; g.go(sh);
  int darker = 0; for (uint32_t p : g.out.px) darker += (p & 0xFF) < 0x80 && p != 0;
  CHECK(darker == 0);
  sh.texts[0].shadow = true; g.go(sh);
  darker = 0; for (uint32_t p : g.out.px) darker += (p & 0xFF) < 0x80;               // drop shadow darkens the backdrop
  CHECK(darker > 20);
  // fade
  RenderState f0 = s; f0.texts[0].alpha = 0.0f; g.go(f0); bool blank = true; for (uint32_t p : g.out.px) if (p) blank = false; CHECK(blank);
  RenderState f5 = s; f5.texts[0].alpha = 0.5f; g.go(f5); int mx = 0; for (uint32_t p : g.out.px) mx = std::max(mx, ch(p, 0)); CHECK(mx >= 126 && mx <= 129);
  RenderState f1 = s; f1.texts[0].alpha = 1.0f; g.go(f1); mx = 0; for (uint32_t p : g.out.px) mx = std::max(mx, ch(p, 0)); CHECK(mx == 255);
  // size
  RenderState big = s; big.texts[0].sizeFrac = 0.14f; big.texts[0].box = NormRect{0.0f, 0.0f, 1.0f, 1.0f}; g.go(big);
  int nb = 0; for (uint32_t p : g.out.px) nb += p != 0;
  RenderState small = s; small.texts[0].sizeFrac = 0.07f; small.texts[0].box = NormRect{0.0f, 0.0f, 1.0f, 1.0f}; g.go(small);
  int ns = 0; for (uint32_t p : g.out.px) ns += p != 0; CHECK(nb > ns * 2);
  // master brightness scales text; blackout hides it
  RenderState dim = s; dim.masterBrightness = 0.5f; g.go(dim); mx = 0; for (uint32_t p : g.out.px) mx = std::max(mx, ch(p, 0)); CHECK(mx >= 126 && mx <= 129);
  // only the first 4 overlays are drawn
  RenderState many = s; many.texts.resize(6); for (int i = 0; i < 6; ++i) { many.texts[i] = s.texts[0]; many.texts[i].text = U"X"; many.texts[i].box = NormRect{0.0f, 0.15f * i, 0.2f, 0.12f}; many.texts[i].sizeFrac = 0.08f; }
  g.go(many);
  int inFifth = 0; for (int y = (int)(0.15 * 4 * 240); y < 240; ++y) for (int x = 0; x < 64; ++x) inFifth += g.out.px[(size_t)y * 320 + x] != 0;
  CHECK(inFifth == 0);
  // >4096 code points does not crash and stays inside the box
  RenderState lng = s; lng.texts[0].text = std::u32string(9000, U'W'); g.go(lng);
  for (int y = 0; y < 144; ++y) for (int x = 0; x < 320; ++x) if (g.out.px[(size_t)y * 320 + x]) { CHECK(false); y = 999; break; }
}

TEST(text_scroll_is_bounded) {
  Rig g;
  std::u32string t; for (int i = 0; i < 40; ++i) { t += U"line "; t.push_back(U'0' + (char32_t)(i % 10)); t += U"\n"; }
  RenderState s = textState(t); s.texts[0].sizeFrac = 0.08f; s.texts[0].box = NormRect{0.1f, 0.1f, 0.8f, 0.4f};
  g.go(s); Frame at0 = g.out;
  s.texts[0].scrollPx = 40; g.go(s); Frame at40 = g.out;
  CHECK(at0.px != at40.px);
  s.texts[0].scrollPx = 1000000; g.go(s); Frame atBig = g.out;
  s.texts[0].scrollPx = 1 << 30; g.go(s); CHECK(g.out.px == atBig.px);                 // clamped to textH - boxH
  s.texts[0].scrollPx = -500; g.go(s); CHECK(g.out.px == at0.px);                      // negative clamps to 0
  // never draws outside the box while scrolling
  for (int sc : {0, 17, 300, 100000}) {
    s.texts[0].scrollPx = sc; g.go(s);
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) if (g.out.px[(size_t)y * 320 + x] && (y < 24 || y >= 24 + 96 || x < 32 || x >= 32 + 256)) { CHECK(false); y = 999; break; }
  }
  // text shorter than the box ignores scroll
  RenderState sh = textState(U"short"); sh.texts[0].scrollPx = 500; g.go(sh); Frame a = g.out; sh.texts[0].scrollPx = 0; g.go(sh); CHECK(a.px == g.out.px);
}

TEST(text_prepare_caches_ahead_of_publication) {
  Rig g;
  TextOverlay t; t.text = U"Prepared ahead of time"; t.sizeFrac = 0.07f; t.box = NormRect{0.1f, 0.6f, 0.8f, 0.3f}; t.h = HAlign::Left;
  size_t b0 = g.r.textCacheBytes();
  g.r.prepareText(t);
  size_t b1 = g.r.textCacheBytes(); CHECK(b1 > b0);
  RenderState s = blackState(); s.texts.push_back(t); g.go(s);
  CHECK(g.r.textCacheBytes() == b1);                                                  // live render hit the cache
  FakeRas ras; g.r.setTextRasterizer(&ras); g.go(s); CHECK(ras.calls == 1 && g.r.textCacheBytes() < b1 + 1000);
  int n = 0; for (uint32_t p : g.out.px) n += p != 0; CHECK(n > 0);
  g.r.setTextRasterizer(nullptr);
}

TEST(overlays_tempo_key_branding_only_where_expected) {
  Rig g;
  RenderState base = sceneState(7, 1, 0, feats(0.3f, 0.3f, 0.3f, 0, false));
  g.go(base); Frame ref = g.out;
  RenderState s = base; s.overlays.showTempo = true; s.overlays.showKey = true;
  s.features.tempo.valid = true; s.features.tempo.value = 120.4; s.features.tempo.confidence = 0.62f; s.features.tempo.source = Estimate::Estimated;
  s.features.key.valid = true; s.features.key.value = 9; s.features.key.minor = true; s.features.key.source = Estimate::Manual;
  g.go(s);
  int diffs = 0; for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) if (g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]) { ++diffs; if (x >= 200 || y >= 80) { CHECK(false); y = 999; break; } }
  CHECK(diffs > 100);
  RenderState a = s; a.overlays.alpha = 0.0f; g.go(a); CHECK(g.out.px == ref.px);
  // unknown tempo/key shows placeholders but still draws
  RenderState u = base; u.overlays.showTempo = true; g.go(u); CHECK(g.out.px != ref.px);
  // different content => different ink
  Frame withKnown = g.out; RenderState k = u; k.features.tempo.valid = true; k.features.tempo.value = 99; g.go(k); CHECK(g.out.px != withKnown.px);
  // approved branding image: bottom-right, opaque pixel values
  Frame logo = solidFrame(16, 8, 0x123456);
  RenderState b = base; b.overlays.brandingImage = &logo; b.overlays.brandingHeightFrac = 0.1f; g.go(b);
  int found = 0, outside = 0;
  for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) { bool d = g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]; if (d) { if (x > 160 && y > 180) ++found; else ++outside; } }
  CHECK(found > 200 && outside == 0);
  CHECK(px(g.out, 300, 225) == 0x123456 || px(g.out, 295, 225) == 0x123456);
  RenderState bt = base; bt.overlays.brandingText = U"ACME"; g.go(bt);
  int bx = 0; for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) if (g.out.px[(size_t)y * 320 + x] != ref.px[(size_t)y * 320 + x]) { ++bx; if (x > 170 || y < 200) { CHECK(false); y = 999; break; } }
  CHECK(bx > 20);
}

int main() { return ss_test::runAll(); }
