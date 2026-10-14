// One-shot generator for the bundled ORIGINAL assets (run manually, NOT part of the build; tests/tools/ is not globbed).
//   g++ -std=c++17 -O1 -Isrc -Ithird_party -Itests tests/tools/gen_assets.cpp src/media/placeholder.cpp \
//       src/media/sha256.cpp src/media/import.cpp src/media/asset_manifest.cpp src/media/stb_impl.cpp \
//       src/media/animation.cpp src/core/clock.cpp -pthread -o gen_assets && ./gen_assets assets
// Everything is procedurally generated here (no third-party pixels). Output is deterministic for a given stb version.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"
#include "media_gif_builder.h"
#include "media/asset_manifest.h"
#include "media/import.h"
#include "media/placeholder.h"
#include "media/sha256.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ss; using namespace ss::media;

static bool writeBytes(const std::string& p, const std::vector<uint8_t>& b) {
  FILE* f = std::fopen(p.c_str(), "wb"); if (!f) return false;
  bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size(); std::fclose(f); return ok;
}
static bool writePng(const std::string& path, const Frame& f) {
  std::vector<uint8_t> rgb((size_t)f.w * f.h * 3);
  for (size_t i = 0; i < f.px.size(); ++i) { rgb[i*3] = (f.px[i] >> 16) & 255; rgb[i*3+1] = (f.px[i] >> 8) & 255; rgb[i*3+2] = f.px[i] & 255; }
  return stbi_write_png(path.c_str(), f.w, f.h, 3, rgb.data(), f.w * 3) != 0;
}

static Frame makeTestCard() {
  const int W = 320, H = 240; Frame f; f.resize(W, H);
  static const uint32_t bars[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    uint32_t c;
    if (y < 160) c = bars[x * 8 / W];
    else if (y < 200) { int g = x * 255 / (W - 1); c = rgb(g, g, g); }          // grayscale ramp
    else c = ((x / 20 + (y - 200) / 20) & 1) ? 0xC0C0C0 : 0x303030;             // checker strip
    if (x % 40 == 0 || y % 40 == 0) c = 0x808080;                                // 40 px alignment grid
    if (x < 2 || y < 2 || x >= W - 2 || y >= H - 2) c = 0xFFFFFF;                // border
    double dx = x - W / 2.0 + 0.5, dy = y - 100.0 + 0.5, r = std::sqrt(dx * dx + dy * dy);
    if (r > 46 && r < 50) c = 0x000000;                                          // centre circle (aspect check)
    if (r > 44 && r <= 46) c = 0xFFFFFF;
    f.px[(size_t)y * W + x] = c;
  }
  return f;
}

static std::vector<uint8_t> makeLoopGif() {
  ss_gif::Spec s; s.w = 64; s.h = 48; s.loop = 0;
  s.palette = {16,16,32,  255,200,0,  140,100,0,  60,50,20};                    // bg, dot, trail1, trail2
  const int N = 12;
  for (int i = 0; i < N; ++i) {
    ss_gif::Frame f; f.x = 0; f.y = 0; f.w = 64; f.h = 48; f.delayCs = 8; f.disposal = 1; f.idx.assign(64 * 48, 0);
    for (int t = 0; t < 3; ++t) {                                                // dot with two fading trail positions
      int k = (i - t + N) % N; double a = 2 * 3.14159265358979 * k / N;
      double cx = 32 + 20 * std::cos(a), cy = 24 + 12 * std::sin(a);
      for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x)
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= (t == 0 ? 25 : 16)) { uint8_t& p = f.idx[(size_t)y * 64 + x]; if (p == 0) p = (uint8_t)(1 + t); }
    }
    s.frames.push_back(f);
  }
  return ss_gif::build(s);
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "assets";
  struct Item { std::string id, file, type, notes; int w, h; int64_t durMs; };
  Frame card = makeTestCard(), miss = makePlaceholderFrame(320, 240);
  std::vector<uint8_t> gif = makeLoopGif();
  if (!writePng(dir + "/testcard_320x240.png", card) || !writePng(dir + "/missing_media_320x240.png", miss) || !writeBytes(dir + "/loop_dot_64x48.gif", gif)) {
    std::fprintf(stderr, "cannot write into %s\n", dir.c_str()); return 1;
  }
  Manifest m; m.schemaVersion = 1; m.generator = "tests/tools/gen_assets.cpp";
  std::vector<Item> items = {
    {"cdl.testcard.320x240", "testcard_320x240.png", "still", "Colour bars, grayscale ramp, 40 px grid, aspect circle. Procedurally generated.", 320, 240, 0},
    {"cdl.placeholder.missing-media.320x240", "missing_media_320x240.png", "still", "Documented 'MISSING MEDIA' checkerboard placeholder (media/placeholder.cpp, REQ-S08). Procedurally generated.", 320, 240, 0},
    {"cdl.loop.dot.64x48", "loop_dot_64x48.gif", "animation", "12-frame looping orbit of a small dot with two-step trail; 80 ms/frame. Procedurally generated.", 64, 48, 960},
  };
  for (const Item& it : items) {
    AssetEntry e; e.id = it.id; e.version = "1.0.0"; e.type = it.type; e.path = it.file;
    e.creator = "Circuit Drift Labs"; e.originalUrl = ""; e.retrievalDate = "2026-10-14";
    e.license = "CC0-1.0"; e.copyright = "Copyright (c) 2026 Circuit Drift Labs; original work dedicated to the public domain under CC0-1.0";
    e.attributionText = "None required (CC0-1.0). Credit appreciated: Circuit Drift Labs"; e.attributionLocation = "licenses/THIRD_PARTY.md (Bundled assets)";
    e.commercialUse = 1; e.modification = 1;
    e.provenanceKind = "original"; e.provenanceNotes = it.notes + " Created by tests/tools/gen_assets.cpp; contains no third-party material.";
    std::string hex; uint64_t sz = 0;
    if (!sha256FileHex(dir + "/" + it.file, hex, &sz).ok()) { std::fprintf(stderr, "hash failed for %s\n", it.file.c_str()); return 1; }
    e.sha256Original = hex; e.sha256Distributed = hex; e.sizeBytes = sz; e.width = it.w; e.height = it.h; e.durationMs = it.durMs;
    e.flash.hasFlashing = false; e.flash.hasStrobe = false; e.flash.maxFlashHz = 0;
    e.flash.notes = it.type == "animation" ? "Small moving dot on a dark field; no full-field luminance change; frame rate 12.5 Hz of motion, not flashing." : "Static image.";
    e.tiers = {"BASE", "STANDARD", "ENHANCED", "SHOW"};
    m.assets.push_back(e);
  }
  // No video decoder is compiled into this repository build: the manifest honestly reports none enabled.
  std::string out = serializeManifest(m);
  FILE* f = std::fopen((dir + "/manifest.json").c_str(), "wb");
  if (!f) return 1;
  std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
  ManifestReport r = verifyManifest(dir);
  std::printf("%s", r.summary().c_str());
  return r.ok ? 0 : 2;
}
