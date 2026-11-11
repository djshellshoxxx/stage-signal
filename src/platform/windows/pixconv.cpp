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

void rgb32ToNv12(const uint32_t* src, int w, int h, uint8_t* y, int yStride, uint8_t* uv, int uvStride) {
  for (int j = 0; j < h; ++j) {
    const uint32_t* r = src + (size_t)j * w; uint8_t* yo = y + (size_t)j * yStride;
    for (int i = 0; i < w; ++i) { int R = (r[i] >> 16) & 255, G = (r[i] >> 8) & 255, B = r[i] & 255; yo[i] = (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16); }
  }
  for (int j = 0; j < h; j += 2) {
    const uint32_t* r0 = src + (size_t)j * w; const uint32_t* r1 = src + (size_t)(j + 1 < h ? j + 1 : j) * w; uint8_t* o = uv + (size_t)(j / 2) * uvStride;
    for (int i = 0; i < w; i += 2) {
      int i1 = i + 1 < w ? i + 1 : i; int R = 0, G = 0, B = 0;
      const uint32_t p[4] = {r0[i], r0[i1], r1[i], r1[i1]};
      for (int k = 0; k < 4; ++k) { R += (p[k] >> 16) & 255; G += (p[k] >> 8) & 255; B += p[k] & 255; }
      R = (R + 2) >> 2; G = (G + 2) >> 2; B = (B + 2) >> 2;
      o[i] = (uint8_t)clamp8(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
      o[i + 1] = (uint8_t)clamp8(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
    }
  }
}
}}}
