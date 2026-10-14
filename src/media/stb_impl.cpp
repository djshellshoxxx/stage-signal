// Single translation unit that compiles stb_image (public domain / MIT-0, v2.30, vendored in third_party/stb).
// Attack surface is reduced to the four beta formats (PNG/JPEG/BMP/GIF); no stdio, no HDR/linear paths.
// SECURITY NOTE: the vendored stb_image v2.30 stbi__load_gif_main() computes `two_back = out - 2*stride`, which
// points BEFORE the frame buffer; a GIF using disposal method 3 on its third frame crashes (verified with ASan).
// We therefore never call stbi_load_gif_from_memory(); ssGifDecode() below drives the same internal frame
// compositor (stbi__gif_load_next) with a correct `two_back` and a pre-sized buffer. Nothing else is changed.
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

// Returns malloc'd RGBA frames (expectFrames * w*h*4) or nullptr on ANY error (truncated/corrupt streams are errors,
// unlike upstream which silently returns the frames decoded so far). Caller frees with std::free.
unsigned char* ssGifDecode(const unsigned char* buf, int len, int expectFrames, int* delaysMs, int* outW, int* outH) {
  if (!buf || len <= 0 || expectFrames <= 0) return nullptr;
  stbi__context s;
  stbi__start_mem(&s, buf, len);
  if (!stbi__gif_test(&s)) return nullptr;
  stbi__gif g;
  std::memset(&g, 0, sizeof(g));
  unsigned char* out = nullptr;
  size_t stride = 0;
  int layers = 0, comp = 0;
  bool ok = false;
  for (;;) {
    stbi_uc* twoBack = (layers >= 2 && out) ? out + (size_t)(layers - 2) * stride : nullptr;
    stbi_uc* u = stbi__gif_load_next(&s, &g, &comp, 4, twoBack);
    if (u == (stbi_uc*)&s) { ok = (layers == expectFrames); break; }   // clean trailer
    if (!u) break;                                                      // error
    if (layers >= expectFrames) break;                                  // more frames than the pre-scan counted
    if (layers == 0) {
      if (g.w <= 0 || g.h <= 0) break;
      stride = (size_t)g.w * (size_t)g.h * 4u;
      out = (unsigned char*)std::malloc(stride * (size_t)expectFrames);
      if (!out) break;
      *outW = g.w; *outH = g.h;
    }
    std::memcpy(out + (size_t)layers * stride, u, stride);
    delaysMs[layers] = g.delay;
    ++layers;
  }
  STBI_FREE(g.out); STBI_FREE(g.history); STBI_FREE(g.background);
  if (!ok) { std::free(out); return nullptr; }
  return out;
}
