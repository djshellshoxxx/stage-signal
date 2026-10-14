// Single translation unit that compiles stb_image (public domain / MIT-0, v2.30, vendored in third_party/stb).
// Attack surface is reduced to the four beta formats (PNG/JPEG/BMP/GIF); no stdio, no HDR/linear paths.
// SECURITY NOTE: the vendored stb_image v2.30 stbi__load_gif_main() computes `two_back = out - 2*stride`, which
// points BEFORE the frame buffer; a GIF using disposal method 3 on its third frame crashes (verified with ASan).
// We therefore never call stbi_load_gif_from_memory(); ssGifDecode() below drives stb's single-frame decoder
// (stbi__gif_load_next) into a pre-sized buffer and performs disposal itself. stb itself is unmodified.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "stb/stb_image.h"
#include "media/stb_gif.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

// Multi-frame GIF decode. stb's stbi__gif_load_next() draws ONE frame's raster (LZW, interlace, palettes, transparency
// skip) into g.out; we do all inter-frame disposal ourselves (stb's own disposal is neutralised by forcing method 1):
//   2 = restore the frame rectangle to transparent (what browsers do), 3 = restore the canvas as it was before that
//   frame was drawn. `info` comes from the bounds-checked pre-scan (rect + disposal per frame).
// Returns malloc'd RGBA frames (expectFrames * w*h*4) or nullptr on ANY error (truncated/corrupt streams are errors,
// unlike upstream which silently returns the frames decoded so far). Caller frees with std::free.
unsigned char* ssGifDecode(const unsigned char* buf, int len, int expectFrames, const SsGifFrameInfo* info, int screenW, int screenH,
                           int* delaysMs) {
  if (!buf || len <= 0 || expectFrames <= 0 || !info || screenW <= 0 || screenH <= 0) return nullptr;
  stbi__context s;
  stbi__start_mem(&s, buf, len);
  if (!stbi__gif_test(&s)) return nullptr;
  stbi__gif g;
  std::memset(&g, 0, sizeof(g));
  const size_t stride = (size_t)screenW * (size_t)screenH * 4u;
  unsigned char* out = (unsigned char*)std::malloc(stride * (size_t)expectFrames);
  unsigned char* saved = (unsigned char*)std::malloc(stride);
  int layers = 0, comp = 0;
  bool ok = false;
  if (out && saved) {
    std::memset(saved, 0, stride);
    for (;;) {
      if (layers >= 1 && g.out) {                                          // dispose the previous frame
        const SsGifFrameInfo& pf = info[layers - 1];
        if (pf.disposal == 2 || pf.disposal == 3)
          for (int y = pf.y; y < pf.y + pf.h && y < screenH; ++y)
            for (int x = pf.x; x < pf.x + pf.w && x < screenW; ++x) {
              size_t o = ((size_t)y * screenW + x) * 4u;
              if (pf.disposal == 2) std::memset(g.out + o, 0, 4); else std::memcpy(g.out + o, saved + o, 4);
            }
      }
      if (layers < expectFrames && info[layers].disposal == 3) {
        if (g.out) std::memcpy(saved, g.out, stride); else std::memset(saved, 0, stride);
      }
      if (g.transparent >= 0) g.pal[g.transparent][3] = 255;               // a GCE applies to the next image only
      g.transparent = -1; g.eflags = (1 << 2); g.delay = 0;                 // dispose=1 (no-op inside stb), no transparency
      stbi_uc* u = stbi__gif_load_next(&s, &g, &comp, 4, nullptr);
      if (u == (stbi_uc*)&s) { ok = (layers == expectFrames); break; }     // clean trailer
      if (!u) break;                                                        // error
      if (layers >= expectFrames) break;                                    // more frames than the pre-scan counted
      if (g.w != screenW || g.h != screenH) break;                          // header disagrees with pre-scan
      std::memcpy(out + (size_t)layers * stride, u, stride);
      delaysMs[layers] = g.delay;
      ++layers;
    }
  }
  STBI_FREE(g.out); STBI_FREE(g.history); STBI_FREE(g.background);
  std::free(saved);
  if (!ok) { std::free(out); return nullptr; }
  return out;
}
