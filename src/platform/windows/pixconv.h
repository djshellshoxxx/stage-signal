// Internal: scalar pixel conversion/scaling (no SIMD, i586-safe). Output pixels are 0x00RRGGBB, row stride == width.
#pragma once
#include <cstdint>
#include <cstddef>
namespace ss { namespace win { namespace pix {
// Full-range BT.601 limited-range YUV -> packed RGB.
uint32_t yuvToRgb(int y, int u, int v);
// bottomUp: memory row 0 is the bottom image row (negative MF stride). stride is the positive byte pitch.
void yuy2ToRgb32(const uint8_t* src, int stride, int w, int h, bool bottomUp, uint32_t* dst);
void nv12ToRgb32(const uint8_t* y, int yStride, const uint8_t* uv, int uvStride, int w, int h, uint32_t* dst);
void bgr24ToRgb32(const uint8_t* src, int stride, int w, int h, bool bottomUp, uint32_t* dst);
void bgrx32ToRgb32(const uint8_t* src, int stride, int w, int h, bool bottomUp, uint32_t* dst);
// Nearest-neighbour scale (fixed point). dst stride == dw.
void scaleNearest(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh);
// Fit (sw,sh) inside (maxW,maxH) keeping aspect; never upscales; result even and >= 2. maxW/maxH <= 0 means unlimited.
void fitSize(int sw, int sh, int maxW, int maxH, int& ow, int& oh);
}}}
