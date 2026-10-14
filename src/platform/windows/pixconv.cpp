#include "pixconv.h"
#include <cstring>
namespace ss { namespace win { namespace pix {

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

uint32_t yuvToRgb(int y, int u, int v) {
  int c = y - 16, d = u - 128, e = v - 128;
  if (c < 0) c = 0;
  int r = clamp8((298 * c + 409 * e + 128) >> 8);
  int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
  int b = clamp8((298 * c + 516 * d + 128) >> 8);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void yuy2ToRgb32(const uint8_t* src, int stride, int w, int h, bool bottomUp, uint32_t* dst) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)(bottomUp ? h - 1 - y : y) * stride;
    uint32_t* d = dst + (size_t)y * w;
    int x = 0;
    for (; x + 1 < w; x += 2, s += 4) {   // Y0 U Y1 V
      d[x] = yuvToRgb(s[0], s[1], s[3]);
      d[x + 1] = yuvToRgb(s[2], s[1], s[3]);
    }
    if (x < w) d[x] = yuvToRgb(s[0], s[1], s[3]);
  }
}

void nv12ToRgb32(const uint8_t* yp, int yStride, const uint8_t* uvp, int uvStride, int w, int h, uint32_t* dst) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* ys = yp + (size_t)y * yStride;
    const uint8_t* uv = uvp + (size_t)(y >> 1) * uvStride;
    uint32_t* d = dst + (size_t)y * w;
    for (int x = 0; x < w; ++x) { int k = x & ~1; d[x] = yuvToRgb(ys[x], uv[k], uv[k + 1]); }
  }
}

void bgr24ToRgb32(const uint8_t* src, int stride, int w, int h, bool bottomUp, uint32_t* dst) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)(bottomUp ? h - 1 - y : y) * stride;
    uint32_t* d = dst + (size_t)y * w;
    for (int x = 0; x < w; ++x, s += 3) d[x] = ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
  }
}

void bgrx32ToRgb32(const uint8_t* src, int stride, int w, int h, bool bottomUp, uint32_t* dst) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)(bottomUp ? h - 1 - y : y) * stride;
    uint32_t* d = dst + (size_t)y * w;
    for (int x = 0; x < w; ++x, s += 4) d[x] = ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
  }
}

void scaleNearest(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
  if (sw == dw && sh == dh) { std::memcpy(dst, src, (size_t)sw * sh * 4); return; }
  const uint32_t xs = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw), ys = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);
  uint32_t fy = ys >> 1;
  for (int y = 0; y < dh; ++y, fy += ys) {
    const uint32_t* row = src + (size_t)(fy >> 16) * sw;
    uint32_t fx = xs >> 1; uint32_t* d = dst + (size_t)y * dw;
    for (int x = 0; x < dw; ++x, fx += xs) d[x] = row[fx >> 16];
  }
}

void fitSize(int sw, int sh, int maxW, int maxH, int& ow, int& oh) {
  ow = sw; oh = sh;
  if (sw <= 0 || sh <= 0) return;
  double s = 1.0;
  if (maxW > 0 && sw > maxW) s = (double)maxW / sw;
  if (maxH > 0 && sh * s > maxH) s = (double)maxH / sh;
  if (s < 1.0) { ow = (int)(sw * s); oh = (int)(sh * s); }
  ow &= ~1; oh &= ~1;
  if (ow < 2) ow = 2;
  if (oh < 2) oh = 2;
}

}}}
