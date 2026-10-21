// Aspect-fit, rotation and normalised-coordinate helpers (REQ-D04, REQ-D07). Pure integer maths.
#pragma once
#include <cstdint>
#include "ss/base/types.hpp"

namespace ss::render {

struct RectI { int x = 0, y = 0, w = 0, h = 0; bool operator==(const RectI& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; } };
enum class FitMode : std::uint8_t { Contain, Cover, Stretch };   // aspect-fit (letterbox), crop-fill, stretch

// Destination rectangle for a src-sized image inside dst. Contain: centred, never exceeds dst. Cover: centred,
// may extend beyond dst (negative x/y). Non-positive sizes give an empty rect.
RectI fitRect(int srcW, int srcH, int dstW, int dstH, FitMode mode);

// Size of a w x h image after rotation by a multiple of 90 degrees (other values are treated as 0).
void rotatedSize(int w, int h, int rotationDeg, int& outW, int& outH);

// Normalised (0..1) rectangle -> pixel rectangle in a w x h target; edges rounded so adjacent rects tile exactly.
RectI normToPixels(float nx, float ny, float nw, float nh, int w, int h);

// Nearest-neighbour blit of src into dst: src rotated clockwise by rotationDeg (0/90/180/270), fitted with `mode`,
// the rest of dst cleared to opaque black. Returns false (dst untouched) for empty buffers. No allocation.
bool blitFit(const ss::PixelBuffer& src, ss::PixelBuffer& dst, FitMode mode, int rotationDeg);

}  // namespace ss::render
