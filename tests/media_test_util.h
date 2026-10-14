// Shared helpers for tests/test_media_*.cpp (header only; not a test itself).
#pragma once
#include "test.h"
#include "core/types.h"
#include "media/import.h"
#include "media_gif_builder.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace mt {

inline std::string tmpDir(const char* tag) {
  static int n = 0;
  auto base = std::filesystem::temp_directory_path() / ("ss_media_" + std::string(tag) + "_" +
              std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007LL) + "_" + std::to_string(n++));
  std::filesystem::create_directories(base);
  return base.generic_string();
}
inline bool writeFile(const std::string& path, const std::vector<uint8_t>& b) {
  FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
  size_t w = b.empty() ? 0 : std::fwrite(b.data(), 1, b.size(), f); std::fclose(f); return w == b.size();
}
inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F> inline bool waitFor(F f, int timeoutMs = 3000) {
  auto t0 = std::chrono::steady_clock::now();
  while (!f()) { if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeoutMs) return false; sleepMs(1); }
  return true;
}

// RGBA8 pattern: (x,y) -> color; encoders go through stb_image_write.
struct Px { uint8_t r, g, b, a; };
using PixFn = Px (*)(int x, int y, int w, int h);
inline Px gradient(int x, int y, int w, int h) { return Px{(uint8_t)(w > 1 ? x * 255 / (w - 1) : 0), (uint8_t)(h > 1 ? y * 255 / (h - 1) : 0), 128, 255}; }
inline Px solidRed(int, int, int, int) { return Px{255, 0, 0, 255}; }
inline Px halfAlpha(int, int, int, int) { return Px{0, 0, 255, 128}; }

inline std::vector<uint8_t> rgba(int w, int h, PixFn fn) {
  std::vector<uint8_t> v((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { Px p = fn(x, y, w, h); uint8_t* q = &v[((size_t)y * w + x) * 4]; q[0] = p.r; q[1] = p.g; q[2] = p.b; q[3] = p.a; }
  return v;
}
inline void sink(void* ctx, void* data, int n) { auto* v = (std::vector<uint8_t>*)ctx; v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + n); }
inline std::vector<uint8_t> png(int w, int h, PixFn fn = gradient, int comp = 4) {
  std::vector<uint8_t> px = rgba(w, h, fn), out;
  if (comp == 3) { std::vector<uint8_t> t((size_t)w * h * 3); for (size_t i = 0; i < (size_t)w * h; ++i) { t[i*3] = px[i*4]; t[i*3+1] = px[i*4+1]; t[i*3+2] = px[i*4+2]; } px = t; }
  stbi_write_png_to_func(sink, &out, w, h, comp, px.data(), w * comp);
  return out;
}
inline std::vector<uint8_t> jpg(int w, int h, PixFn fn = gradient) {
  std::vector<uint8_t> px = rgba(w, h, fn), out; stbi_write_jpg_to_func(sink, &out, w, h, 4, px.data(), 90); return out;
}
inline std::vector<uint8_t> bmp(int w, int h, PixFn fn = gradient) {
  std::vector<uint8_t> px = rgba(w, h, fn), out; stbi_write_bmp_to_func(sink, &out, w, h, 4, px.data()); return out;
}
inline void be32(std::vector<uint8_t>& v, size_t off, uint32_t x) { v[off] = x >> 24; v[off+1] = x >> 16; v[off+2] = x >> 8; v[off+3] = x; }

// A structurally complete PNG whose IHDR claims w x h but whose IDAT is a few junk bytes (tiny file, huge claim).
inline std::vector<uint8_t> pngClaiming(uint32_t w, uint32_t h) {
  std::vector<uint8_t> v = png(8, 8);
  be32(v, 16, w); be32(v, 20, h);
  return v;
}

// Palette helpers for GIF tests.
inline std::vector<uint8_t> pal4() { return {255,0,0,  0,255,0,  0,0,255,  255,255,255}; }
inline ss_gif::Frame gframe(int x, int y, int w, int h, int idx, int delayCs, int disposal, int transparent = -1) {
  ss_gif::Frame f; f.x = x; f.y = y; f.w = w; f.h = h; f.idx.assign((size_t)w * h, (uint8_t)idx); f.delayCs = delayCs; f.disposal = disposal; f.transparent = transparent; return f;
}
inline uint32_t argbAt(const ss::media::DecodedGif& g, int frame, int x, int y) { return g.argb[((size_t)frame * g.h + y) * g.w + x]; }

} // namespace mt

// Counts the largest single C++ allocation (stb uses malloc, so tests also assert on error CODES: a limit rejection
// must be ResourceExhausted, never a DecodeFailure that would imply pixel decoding started).
