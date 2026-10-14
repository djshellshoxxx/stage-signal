// MOD-07 import gate: paths, type gating, header probes, limits-before-allocation, stills, GIF, timing, crop.
#include "media_test_util.h"
#include "media/animation.h"
#include "stb/stb_image.h"
#include <cstdlib>
#include <new>

using namespace ss; using namespace ss::media;

// Track the largest single operator-new request so tests can prove rejected inputs never allocate pixel buffers.
static size_t g_maxNew = 0;
void* operator new(size_t n) { if (n > g_maxNew) g_maxNew = n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static ImportLimits baseLim() { return limitsFor(Tier::Base); }
static ImportLimits stdLim() { return limitsFor(Tier::Standard); }
static Status decodeStill(const std::vector<uint8_t>& b, const char* name, const ImportLimits& l, DecodedStill& o) { return validateAndDecodeStill(b.data(), b.size(), name, l, o); }
static Status decodeGif(const std::vector<uint8_t>& b, const ImportLimits& l, DecodedGif& o, const char* name = "a.gif") { return validateAndDecodeGif(b.data(), b.size(), name, l, o); }

TEST(path_rejects_urls_unc_traversal) {
  const char* bad[] = {"http://example.com/a.png", "https://x/a.png", "file:///etc/passwd", "file://host/a.png", "data:image/png;base64,AAAA",
    "ftp://x/a.png", "\\\\server\\share\\a.png", "//server/share/a.png", "\\\\?\\C:\\a.png", "\\\\.\\pipe\\x", "../a.png", "a/../b.png",
    "..\\a.png", "a\\..\\..\\b.png", "C:foo.png", "a.png:stream", "a.png::$DATA", "", "tab\there.png", "CON.png", "dir/nul.png"};
  for (const char* p : bad) { bool ok = validateLocalPath(p).ok(); CHECK(!ok); if (ok) std::printf("  accepted: %s\n", p); }
  std::string nul("bad\0name.png", 12); CHECK(!validateLocalPath(nul).ok());
  CHECK(validateLocalPath("media/a.png").ok());
  CHECK(validateLocalPath("/tmp/ss/a.png").ok());
  CHECK(validateLocalPath("C:\\Users\\x\\a.png").ok());
  CHECK(validateLocalPath("C:/Users/x/a.png").ok());
  CHECK(validateLocalPath("dir.with.dots/a..b.png").ok());
  CHECK(validateLocalPath("..").code == Err::InvalidArgument);
}

TEST(extension_and_magic_gate) {
  AssetKind k;
  CHECK(classifyExtension("a.PNG", k).ok() && k == AssetKind::Still);
  CHECK(classifyExtension("a.jpeg", k).ok() && k == AssetKind::Still);
  CHECK(classifyExtension("a.gif", k).ok() && k == AssetKind::Gif);
  CHECK(classifyExtension("a.mkv", k).ok() && k == AssetKind::Video);
  const char* rej[] = {"a.svg", "a.html", "a.htm", "a.js", "a.xml", "a.exe", "a", "a.png.svg", "a.webp", "a.tga", "a.psd"};
  for (const char* r : rej) CHECK(!classifyExtension(r, k).ok());
  std::vector<uint8_t> p = png(4, 4), j = jpg(8, 8), b = bmp(4, 4);
  CHECK(sniffContainer(p.data(), p.size()) == Container::Png);
  CHECK(sniffContainer(j.data(), j.size()) == Container::Jpeg);
  CHECK(sniffContainer(b.data(), b.size()) == Container::Bmp);
  // extension must agree with magic
  CHECK(!checkExtensionAndMagic("a.jpg", p.data(), p.size(), k).ok());
  CHECK(!checkExtensionAndMagic("a.png", j.data(), j.size(), k).ok());
  CHECK(!checkExtensionAndMagic("a.gif", p.data(), p.size(), k).ok());
  CHECK(checkExtensionAndMagic("a.png", p.data(), p.size(), k).ok());
  // markup / script disguised with an allowed extension
  const char* svg = "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>";
  const char* html = "  \n<!DOCTYPE html><html><script>x</script></html>";
  const char* scr = "<script>evil()</script>";
  for (const char* t : {svg, html, scr}) {
    CHECK(sniffContainer((const uint8_t*)t, std::strlen(t)) == Container::Markup);
    DecodedStill o; CHECK(validateAndDecodeStill((const uint8_t*)t, std::strlen(t), "photo.png", baseLim(), o).code == Err::InvalidArgument);
    CHECK(validateAndDecodeStill((const uint8_t*)t, std::strlen(t), "", baseLim(), o).code == Err::InvalidArgument);
  }
  // PNG signature in a file named .svg
  DecodedStill o; CHECK(!decodeStill(p, "pic.svg", baseLim(), o).ok());
  // unknown garbage
  std::vector<uint8_t> junk(64, 0x41); CHECK(!decodeStill(junk, "a.png", baseLim(), o).ok());
}

TEST(still_decodes_png_jpeg_bmp) {
  DecodedStill o;
  CHECK(decodeStill(png(32, 16), "a.png", baseLim(), o).ok());
  CHECK(o.w == 32 && o.h == 16 && o.argb.size() == 512 && !o.hasAlpha && !o.downscaled);
  CHECK((o.argb[0] >> 24) == 255);
  CHECK((o.argb[31] & 0xFF0000) >> 16 == 255);                     // gradient: red at right edge
  CHECK(decodeStill(png(16, 16, mt::halfAlpha), "a.png", baseLim(), o).ok() && o.hasAlpha && (o.argb[0] >> 24) == 128);
  Frame f = frameFromArgb(o.argb.data(), o.w, o.h, 0xFFFFFF);       // flatten blue@50% over white
  CHECK((f.px[0] >> 24) == 0);
  CHECK(((f.px[0] >> 16) & 255) >= 126 && ((f.px[0] >> 16) & 255) <= 128 && (f.px[0] & 255) == 255);
  CHECK(decodeStill(jpg(40, 24), "a.JPG", baseLim(), o).ok() && o.w == 40 && o.h == 24);
  CHECK(decodeStill(jpg(40, 24), "a.jpeg", baseLim(), o).ok());
  CHECK(decodeStill(bmp(10, 7), "a.bmp", baseLim(), o).ok() && o.w == 10 && o.h == 7);
  CHECK(decodeStill(png(9, 9, mt::gradient, 3), "a.png", baseLim(), o).ok());
  CHECK(decodeStill(png(9, 9), "", baseLim(), o).ok());             // in-memory: magic alone decides
}

TEST(still_limits_checked_before_allocation) {
  DecodedStill o;
  // tiny file claiming absurd dimensions: must be ResourceExhausted (limit check), never DecodeFailure (decode started)
  g_maxNew = 0;
  uint32_t dims[][2] = {{100000, 100000}, {4097, 16}, {16, 4097}, {2049, 2049}, {65536, 65536}, {0x7FFFFFFF, 1}};
  for (auto& d : dims) {
    Status s = decodeStill(mt::pngClaiming(d[0], d[1]), "a.png", baseLim(), o);
    CHECK(s.code == Err::ResourceExhausted);
    if (s.code != Err::ResourceExhausted) std::printf("  dims %ux%u -> %s %s\n", d[0], d[1], errName(s.code), s.detail.c_str());
    CHECK(o.argb.empty());
  }
  CHECK(g_maxNew < (1u << 20));                                    // no big C++ buffer was ever requested
  CHECK(decodeStill(mt::pngClaiming(5000, 5000), "a.png", stdLim(), o).code == Err::ResourceExhausted);
  CHECK(decodeStill(mt::pngClaiming(4097, 1), "a.png", stdLim(), o).code == Err::ResourceExhausted);
  // decoded byte limit independent of dimensions
  ImportLimits tight = stdLim(); tight.maxSrcDecodedBytes = 1000;
  CHECK(decodeStill(png(20, 20), "a.png", tight, o).code == Err::ResourceExhausted);   // 20*20*4 = 1600 > 1000
  CHECK(decodeStill(png(10, 10), "a.png", tight, o).ok());                              // 400 <= 1000
  // encoded size limit
  ImportLimits small = stdLim(); small.maxFileBytes = 50;
  CHECK(decodeStill(png(20, 20), "a.png", small, o).code == Err::ResourceExhausted);
  // JPEG / BMP headers rejected the same way
  std::vector<uint8_t> j = mt::jpg(16, 16);
  for (size_t i = 0; i + 9 < j.size(); ++i) if (j[i] == 0xFF && j[i+1] == 0xC0) { j[i+5] = 0xFF; j[i+6] = 0xF0; j[i+7] = 0xFF; j[i+8] = 0xF0; break; }
  CHECK(decodeStill(j, "a.jpg", baseLim(), o).code == Err::ResourceExhausted);
  std::vector<uint8_t> b = mt::bmp(8, 8); mt::be32(b, 18, 0); b[18] = 0x00; b[19] = 0x40; b[20] = 0; b[21] = 0;   // width = 0x4000 (LE16), others 0
  b[18] = 0; b[19] = 0x40; b[20] = 0; b[21] = 0;
  CHECK(!decodeStill(b, "a.bmp", baseLim(), o).ok());
}

TEST(still_corrupt_truncated_zero_dim_rejected) {
  DecodedStill o;
  std::vector<uint8_t> p = mt::png(64, 64), j = mt::jpg(64, 64), b = mt::bmp(32, 32);
  // zero dimensions
  std::vector<uint8_t> z = p; mt::be32(z, 16, 0); CHECK(decodeStill(z, "a.png", baseLim(), o).code == Err::DecodeFailure);
  z = p; mt::be32(z, 20, 0); CHECK(decodeStill(z, "a.png", baseLim(), o).code == Err::DecodeFailure);
  z = b; z[18] = z[19] = z[20] = z[21] = 0; CHECK(!decodeStill(z, "a.bmp", baseLim(), o).ok());
  // truncation at many points
  for (size_t cut : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(20), size_t(33), p.size() / 2, p.size() - 1, p.size() - 12}) {
    std::vector<uint8_t> t(p.begin(), p.begin() + (cut < p.size() ? cut : p.size()));
    CHECK(!decodeStill(t, "a.png", baseLim(), o).ok());
  }
  for (size_t cut : {size_t(2), size_t(3), size_t(100), j.size() / 2, j.size() - 2}) { std::vector<uint8_t> t(j.begin(), j.begin() + cut); CHECK(!decodeStill(t, "a.jpg", baseLim(), o).ok()); }
  for (size_t cut : {size_t(2), size_t(14), size_t(26), size_t(60), b.size() / 2, b.size() - 1}) { std::vector<uint8_t> t(b.begin(), b.begin() + cut); CHECK(!decodeStill(t, "a.bmp", baseLim(), o).ok()); }
  // corrupted IDAT payload (bad zlib stream)
  z = p; for (size_t i = 60; i < 90 && i < z.size() - 20; ++i) z[i] ^= 0xA5;
  Status s = decodeStill(z, "a.png", baseLim(), o); CHECK(s.code == Err::DecodeFailure || s.ok());   // never crashes
  CHECK(!decodeStill(std::vector<uint8_t>(), "a.png", baseLim(), o).ok());
  // deterministic mutation fuzz: must never crash or throw
  uint32_t seed = 12345; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  int okCount = 0;
  for (int it = 0; it < 300; ++it) {
    const std::vector<uint8_t>& src = (it % 3 == 0) ? p : (it % 3 == 1) ? j : b;
    std::vector<uint8_t> m = src; int flips = 1 + (int)(rnd() % 8);
    for (int f = 0; f < flips; ++f) m[rnd() % m.size()] = (uint8_t)rnd();
    if (rnd() % 4 == 0) m.resize(rnd() % m.size());
    const char* nm = (it % 3 == 0) ? "a.png" : (it % 3 == 1) ? "a.jpg" : "a.bmp";
    Status st = decodeStill(m, nm, baseLim(), o); if (st.ok()) ++okCount;
  }
  CHECK(okCount >= 0);
}

TEST(base_stills_preprocessed_to_800x600) {
  DecodedStill o;
  CHECK(decodeStill(mt::png(1600, 1200), "a.png", baseLim(), o).ok());
  CHECK(o.w == 800 && o.h == 600 && o.downscaled && o.srcW == 1600 && o.srcH == 1200 && !o.warnings.empty());
  CHECK(decodeStill(mt::png(2000, 500), "a.png", baseLim(), o).ok() && o.w == 800 && o.h == 200);       // aspect preserved
  CHECK(decodeStill(mt::png(500, 2000), "a.png", baseLim(), o).ok() && o.h == 600 && o.w == 150);
  CHECK(decodeStill(mt::png(800, 600), "a.png", baseLim(), o).ok() && o.w == 800 && !o.downscaled);
  CHECK(decodeStill(mt::png(320, 240), "a.png", baseLim(), o).ok() && o.w == 320 && !o.downscaled);
  CHECK(decodeStill(mt::pngClaiming(4000, 4000), "a.png", baseLim(), o).code == Err::ResourceExhausted);  // beyond BASE source cap
  // box filter averages: 2x2 checker of black/white -> mid grey
  std::vector<uint32_t> src = {0xFF000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF000000u}, dst;
  downscaleBoxArgb(src.data(), 2, 2, 1, 1, dst);
  CHECK(dst.size() == 1 && ((dst[0] >> 16) & 255) >= 127 && ((dst[0] >> 16) & 255) <= 128 && (dst[0] >> 24) == 255);
  // Standard tier keeps full size up to 4096x4096 / 64 MiB
  CHECK(decodeStill(mt::png(2048, 2048, mt::solidRed), "a.png", stdLim(), o).ok() && o.w == 2048 && !o.downscaled);
}

TEST(standard_accepts_exactly_4096_square) {
  DecodedStill o;
  Status s = decodeStill(mt::png(4096, 4096, mt::solidRed), "a.png", stdLim(), o);
  CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str());
  CHECK(o.w == 4096 && o.h == 4096 && o.bytes() == 64ull << 20);
}

// ---- GIF ----------------------------------------------------------------------------------------------------------
TEST(gif_delays_clamped_with_warning) {
  ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4(); s.loop = 0;
  s.frames.push_back(mt::gframe(0, 0, 4, 4, 0, 0, 1));    // 0 cs
  s.frames.push_back(mt::gframe(0, 0, 4, 4, 1, 1, 1));    // 10 ms
  s.frames.push_back(mt::gframe(0, 0, 4, 4, 2, 2, 1));    // 20 ms (not clamped)
  s.frames.push_back(mt::gframe(0, 0, 4, 4, 3, 7, 1));    // 70 ms
  s.frames.push_back(mt::gframe(0, 0, 4, 4, 0, 500, 1));  // 5000 ms
  DecodedGif g; Status st = decodeGif(ss_gif::build(s), baseLim(), g);
  CHECK(st.ok()); if (!st.ok()) std::printf("  %s\n", st.detail.c_str());
  CHECK(g.frameCount == 5 && g.delayMs.size() == 5);
  CHECK(g.delayMs[0] == 20 && g.delayMs[1] == 20 && g.delayMs[2] == 20 && g.delayMs[3] == 70 && g.delayMs[4] == 5000);
  CHECK(g.rawDelayMs[0] == 0 && g.rawDelayMs[1] == 10);
  CHECK(g.warnings.size() == 1 && g.warnings[0].find("clamped") != std::string::npos && g.warnings[0].find("2 ") == 0);
  CHECK(g.loopCount == 0);
  CHECK(g.totalMs() == 20 + 20 + 20 + 70 + 5000);
  // frame without a Graphic Control Extension has delay 0 -> clamped
  s.frames.clear(); s.frames.push_back(mt::gframe(0, 0, 4, 4, 0, 10, 1)); s.frames[0].emitGce = false; s.loop = -1;
  CHECK(decodeGif(ss_gif::build(s), baseLim(), g).ok() && g.delayMs[0] == 20 && g.loopCount == -1);
}

TEST(gif_disposal_and_transparency) {
  const uint32_t R = 0xFFFF0000, G = 0xFF00FF00, B = 0xFF0000FF, W = 0xFFFFFFFF, T = 0;
  DecodedGif g;
  { // method 1 (do not dispose): partial frame draws over the previous composite
    ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4();
    s.frames = {mt::gframe(0,0,4,4,0,10,1), mt::gframe(0,0,2,2,1,10,1)};
    CHECK(decodeGif(ss_gif::build(s), baseLim(), g).ok() && g.frameCount == 2);
    CHECK(mt::argbAt(g,0,3,3) == R && mt::argbAt(g,1,0,0) == G && mt::argbAt(g,1,1,1) == G && mt::argbAt(g,1,2,2) == R && mt::argbAt(g,1,3,0) == R);
  }
  { // method 2 (restore to background): disposed area becomes transparent for the next frame
    ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4();
    s.frames = {mt::gframe(0,0,4,4,0,10,1), mt::gframe(0,0,2,2,1,10,2), mt::gframe(3,3,1,1,2,10,1)};
    CHECK(decodeGif(ss_gif::build(s), baseLim(), g).ok() && g.frameCount == 3);
    CHECK(mt::argbAt(g,1,0,0) == G && mt::argbAt(g,1,3,3) == R);
    CHECK(mt::argbAt(g,2,0,0) == T && (mt::argbAt(g,2,1,1) >> 24) == 0);      // green patch disposed -> transparent
    CHECK(mt::argbAt(g,2,3,3) == B && mt::argbAt(g,2,3,0) == R);
  }
  { // method 3 (restore previous) on the THIRD frame: this crashes upstream stbi_load_gif_from_memory (two_back bug)
    ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4();
    s.frames = {mt::gframe(0,0,4,4,0,10,1), mt::gframe(0,0,2,2,1,10,3), mt::gframe(3,3,1,1,2,10,1), mt::gframe(1,1,2,2,3,10,1)};
    Status st = decodeGif(ss_gif::build(s), baseLim(), g); CHECK(st.ok());
    CHECK(g.frameCount == 4);
    CHECK(mt::argbAt(g,1,0,0) == G);
    CHECK(mt::argbAt(g,2,0,0) == R && mt::argbAt(g,2,1,1) == R && mt::argbAt(g,2,3,3) == B);   // green patch reverted to red
    CHECK(mt::argbAt(g,3,1,1) == W && mt::argbAt(g,3,0,0) == R && mt::argbAt(g,3,3,3) == B);
  }
  { // disposal 3 on the second frame has nothing to restore -> treated as background (stb behaviour, documented)
    ss_gif::Spec s; s.w = 2; s.h = 2; s.palette = mt::pal4();
    s.frames = {mt::gframe(0,0,2,2,0,10,3), mt::gframe(0,0,1,1,1,10,1)};
    CHECK(decodeGif(ss_gif::build(s), baseLim(), g).ok() && mt::argbAt(g,1,0,0) == G);
  }
  { // transparent index: previous composite shows through; first-frame transparent pixels stay transparent
    ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4();
    ss_gif::Frame f0 = mt::gframe(0,0,4,4,0,10,1);
    ss_gif::Frame f1 = mt::gframe(0,0,4,4,1,10,1, /*transparent=*/1);        // all pixels use the transparent index
    f1.idx[5] = 2;                                                            // except (1,1) = blue
    s.frames = {f0, f1};
    CHECK(decodeGif(ss_gif::build(s), baseLim(), g).ok() && g.hasTransparency);
    CHECK(mt::argbAt(g,1,0,0) == R && mt::argbAt(g,1,1,1) == B && mt::argbAt(g,1,3,3) == R);
    ss_gif::Spec s2; s2.w = 2; s2.h = 2; s2.palette = mt::pal4();
    ss_gif::Frame t0 = mt::gframe(0,0,2,2,1,10,1, 1); t0.idx[3] = 2;
    s2.frames = {t0};
    CHECK(decodeGif(ss_gif::build(s2), baseLim(), g).ok());
    CHECK(mt::argbAt(g,0,0,0) == T && mt::argbAt(g,0,1,1) == B);
    Frame fl = frameFromArgb(g.argb.data(), 2, 2, 0x00FF00);                   // flattened onto a green matte
    CHECK(fl.px[0] == 0x00FF00 && fl.px[3] == 0x0000FF);
  }
  { // local color table + sub-rectangle at an offset
    ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4();
    ss_gif::Frame f = mt::gframe(1,2,2,1,0,10,1); f.localPalette = {10,20,30, 200,100,50}; f.idx = {1, 0};
    s.frames = {mt::gframe(0,0,4,4,3,10,1), f};
    CHECK(decodeGif(ss_gif::build(s), baseLim(), g).ok());
    CHECK(mt::argbAt(g,1,1,2) == 0xFFC86432 && mt::argbAt(g,1,2,2) == 0xFF0A141E && mt::argbAt(g,1,0,0) == W);
  }
}

TEST(gif_matches_stb_reference_when_no_disposal3) {
  ss_gif::Spec s; s.w = 16; s.h = 12; s.palette = mt::pal4(); s.loop = 0;
  for (int i = 0; i < 6; ++i) { auto f = mt::gframe(i, i, 6, 5, i % 4, 5 + i, (i % 2) ? 2 : 1, (i == 3) ? 2 : -1); s.frames.push_back(f); }
  std::vector<uint8_t> b = ss_gif::build(s);
  int x, y, z, c; int* d = nullptr;
  unsigned char* ref = stbi_load_gif_from_memory(b.data(), (int)b.size(), &d, &x, &y, &z, &c, 4);
  CHECK(ref != nullptr && z == 6);
  DecodedGif g; CHECK(decodeGif(b, baseLim(), g).ok() && (int)g.frameCount == z);
  if (ref && g.frameCount == (uint32_t)z) {
    bool same = true;
    for (size_t i = 0; i < (size_t)x * y * z; ++i) {
      const unsigned char* q = ref + i * 4; uint32_t a = q[3];
      uint32_t want = a == 0 ? 0u : ((a << 24) | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2]);
      if (want != g.argb[i]) same = false;
    }
    CHECK(same);
    for (int i = 0; i < z; ++i) CHECK(g.rawDelayMs[i] == d[i]);
  }
  stbi_image_free(ref); stbi_image_free(d);
}

TEST(gif_limits_and_corruption) {
  DecodedGif g;
  auto many = [&](int n) { ss_gif::Spec s; s.w = 1; s.h = 1; s.palette = mt::pal4(); for (int i = 0; i < n; ++i) s.frames.push_back(mt::gframe(0,0,1,1,i % 4,2,1)); return ss_gif::build(s); };
  CHECK(decodeGif(many(1000), baseLim(), g).ok() && g.frameCount == 1000);
  g_maxNew = 0;
  CHECK(decodeGif(many(1001), baseLim(), g).code == Err::ResourceExhausted);
  CHECK(decodeGif(many(1001), stdLim(), g).code == Err::ResourceExhausted);
  // decoded budget: BASE 8 MiB -> 100x100x4 = 40 KB per frame, 300 frames = 12 MB rejected before any frame buffer
  { ss_gif::Spec s; s.w = 100; s.h = 100; s.palette = mt::pal4(); for (int i = 0; i < 300; ++i) s.frames.push_back(mt::gframe(0,0,1,1,i % 4,2,1));
    std::vector<uint8_t> b = ss_gif::build(s);
    Status st = decodeGif(b, baseLim(), g); CHECK(st.code == Err::ResourceExhausted);
    CHECK(g_maxNew < (1u << 20));
    CHECK(decodeGif(b, stdLim(), g).ok());                         // 12 MB fits the 64 MiB higher-tier budget
  }
  // oversize logical screen
  { ss_gif::Spec s; s.w = 5000; s.h = 5000; s.palette = mt::pal4(); s.frames.push_back(mt::gframe(0,0,1,1,0,2,1));
    CHECK(decodeGif(ss_gif::build(s), stdLim(), g).code == Err::ResourceExhausted); }
  std::vector<uint8_t> ok = many(5);
  { std::vector<uint8_t> z = ok; z[6] = z[7] = 0; CHECK(decodeGif(z, baseLim(), g).code == Err::DecodeFailure); }    // zero width
  { std::vector<uint8_t> z = ok; z[8] = z[9] = 0; CHECK(!decodeGif(z, baseLim(), g).ok()); }
  { std::vector<uint8_t> z(ok.begin(), ok.end() - 1); CHECK(!decodeGif(z, baseLim(), g).ok()); }                      // no trailer
  for (size_t cut : {size_t(0), size_t(5), size_t(12), size_t(20), ok.size() / 2}) { std::vector<uint8_t> z(ok.begin(), ok.begin() + cut); CHECK(!decodeGif(z, baseLim(), g).ok()); }
  { ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4(); s.frames.push_back(mt::gframe(2,2,4,4,0,2,1));            // frame outside screen
    CHECK(!decodeGif(ss_gif::build(s), baseLim(), g).ok()); }
  { ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4(); CHECK(!decodeGif(ss_gif::build(s), baseLim(), g).ok()); }  // no frames
  CHECK(!decodeGif(mt::png(4, 4), baseLim(), g, "a.gif").ok());
  CHECK(!decodeGif(ok, baseLim(), g, "a.png").ok());
  uint32_t seed = 777; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  ss_gif::Spec fs; fs.w = 8; fs.h = 8; fs.palette = mt::pal4(); fs.loop = 0;
  for (int i = 0; i < 5; ++i) fs.frames.push_back(mt::gframe(i, 0, 4, 4, i % 4, 3, i % 4));
  std::vector<uint8_t> base = ss_gif::build(fs);
  for (int it = 0; it < 400; ++it) {
    std::vector<uint8_t> m = base; int flips = 1 + (int)(rnd() % 6);
    for (int f = 0; f < flips; ++f) m[rnd() % m.size()] = (uint8_t)rnd();
    if (rnd() % 5 == 0) m.resize(1 + rnd() % m.size());
    (void)decodeGif(m, baseLim(), g);                                                                                    // must not crash
  }
}

TEST(gif_lzw_roundtrip_large_image) {
  ss_gif::Spec s; s.w = 120; s.h = 90; for (int i = 0; i < 256; ++i) { s.palette.push_back((uint8_t)i); s.palette.push_back((uint8_t)(255 - i)); s.palette.push_back((uint8_t)(i * 7)); }
  ss_gif::Frame f; f.x = 0; f.y = 0; f.w = 120; f.h = 90; f.delayCs = 5; f.disposal = 1;
  uint32_t seed = 99; for (int i = 0; i < 120 * 90; ++i) { seed = seed * 1103515245u + 12345u; f.idx.push_back((uint8_t)((seed >> 16) % ((i / 400) % 2 ? 256 : 5))); }
  s.frames.push_back(f);
  DecodedGif g; CHECK(decodeGif(ss_gif::build(s), stdLim(), g).ok());
  bool same = g.argb.size() == f.idx.size();
  for (size_t i = 0; same && i < f.idx.size(); ++i) { uint32_t k = f.idx[i]; uint32_t want = 0xFF000000u | (k << 16) | ((255 - k) << 8) | ((k * 7) & 255); if (g.argb[i] != want) same = false; }
  CHECK(same);
}

// ---- timing / crop -----------------------------------------------------------------------------------------------
TEST(animation_media_clock_indexing) {
  std::vector<Ns> ends = cumulativeEnds({100, 50, 200});              // 100, 150, 350 ms
  CHECK(ends.size() == 3 && ends[2] == 350 * kNsPerMs);
  CHECK(animFrameIndex(ends, 0, true) == 0);
  CHECK(animFrameIndex(ends, 99 * kNsPerMs, true) == 0);
  CHECK(animFrameIndex(ends, 100 * kNsPerMs, true) == 1);
  CHECK(animFrameIndex(ends, 149 * kNsPerMs, true) == 1);
  CHECK(animFrameIndex(ends, 150 * kNsPerMs, true) == 2);
  CHECK(animFrameIndex(ends, 349 * kNsPerMs, true) == 2);
  CHECK(animFrameIndex(ends, 350 * kNsPerMs, true) == 0);              // wraps
  CHECK(animFrameIndex(ends, 3 * 350 * kNsPerMs + 120 * kNsPerMs, true) == 1);
  CHECK(animFrameIndex(ends, 350 * kNsPerMs, false) == 2);             // holds last
  CHECK(animFrameIndex(ends, 99999 * kNsPerMs, false) == 2);
  CHECK(animFrameIndex(ends, -5, true) == 0);
  CHECK(animFrameIndex({}, 5, true) == 0);
  CHECK_NEAR(clampSpeed(10, kGifSpeedMin, kGifSpeedMax), 4.0, 1e-9);
  CHECK_NEAR(clampSpeed(0.01, kGifSpeedMin, kGifSpeedMax), 0.25, 1e-9);
  CHECK_NEAR(clampSpeed(3, kVideoSpeedMin, kVideoSpeedMax), 2.0, 1e-9);
  CHECK_NEAR(clampSpeed(-1, kVideoSpeedMin, kVideoSpeedMax), 1.0, 1e-9);
  CHECK_NEAR(clampSpeed(std::nan(""), kGifSpeedMin, kGifSpeedMax), 1.0, 1e-9);
  MediaCursor c;                                                       // speed change mid-stream does not jump
  CHECK(c.advance(1000, 1.0) == 1000);
  CHECK(c.advance(2000, 1.0) == 2000);
  CHECK(c.advance(3000, 2.0) == 4000);
  CHECK(c.advance(4000, 2.0) == 6000);
}

TEST(crop_and_flatten) {
  Frame f; f.resize(10, 10); for (int y = 0; y < 10; ++y) for (int x = 0; x < 10; ++x) f.px[y * 10 + x] = rgb(x * 20, y * 20, 0);
  CropRect c; c.x = 0.5f; c.y = 0.25f; c.w = 0.5f; c.h = 0.5f;
  CHECK(validateCrop(c).ok());
  Frame o = cropFrame(f, c);
  CHECK(o.w == 5 && o.h == 5 && o.px[0] == f.px[3 * 10 + 5] && o.px[24] == f.px[7 * 10 + 9]);
  CHECK(cropFrame(f, CropRect()).w == 10);
  CropRect bad[] = {{-0.1f, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 1.5f, 1}, {0.9f, 0, 0.5f, 1}, {1, 0, 0.1f, 1}, {0, 0, std::nanf(""), 1}};
  for (auto& b : bad) CHECK(!validateCrop(b).ok());
  CropRect tiny; tiny.x = 0.99f; tiny.y = 0.99f; tiny.w = 0.01f; tiny.h = 0.01f; CHECK(validateCrop(tiny).ok());
  Frame t = cropFrame(f, tiny); CHECK(t.w >= 1 && t.h >= 1 && t.w <= 10);
}

int main() { return ss_test::runAll(); }
