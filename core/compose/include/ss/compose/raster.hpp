// Software raster primitives. No allocation; all integer math (deterministic).
#pragma once
#include "ss/compose/pixel.hpp"

namespace ss::compose {

// Resize without releasing capacity (no allocation while w*h*4 <= capacity). Contents become zero where grown.
void resizeKeep(PixelBuffer& b, std::uint32_t w, std::uint32_t h);
bool validDims(std::uint32_t w, std::uint32_t h);  // <= kMaxDimension per edge and <= kMaxBufferBytes

void fill(PixelBuffer& dst, Px colour);
void fillRect(PixelBuffer& dst, Rect r, Px colour, bool blendOver);  // clipped

// Normalised -> pixel rectangle (clamped to the canvas; non-empty rect stays at least 1x1).
Rect toPixelRect(RectF r, int canvasW, int canvasH);

struct FitResult { Rect src; Rect dst; };
// Where a (srcW x srcH) image lands inside `region` for each fit mode (REQ-D04/D07).
//  Fit: whole image visible, centred, aspect kept (bars of background remain).
//  Crop: region filled, centred source window with the region aspect.
//  Stretch: region filled, aspect distorted.
FitResult computeFit(int srcW, int srcH, Rect region, FitMode mode);

struct DrawParams {
    Sampling sampling = Sampling::Bilinear;
    bool blendOver = true;          // false = replace destination pixels
    std::uint32_t opacity256 = 256; // 0..256 applied to the sampled premultiplied pixel
    bool maskTint = false;          // treat source as a coverage mask (alpha only) tinted with `tint`
    Rgb tint{255, 255, 255};
    bool flipX = false;
};
// Scale srcRect of src into dstRect of dst (dstRect may extend beyond dst; it is clipped).
// Corner-aligned sampling: dst first/last pixel map to src first/last pixel, so a 1 px reference border of the
// source is always visible on the outermost destination row/column (REQ-D04).
void drawScaled(const PixelBuffer& src, Rect srcRect, PixelBuffer& dst, Rect dstRect, const DrawParams& p);

// Fit `src` (optionally a normalised sub-rectangle `crop` of it) into `region` of dst.
void drawFitted(const PixelBuffer& src, RectF crop, PixelBuffer& dst, Rect region, FitMode mode, const DrawParams& p);

// Copy src into dst rotated clockwise by rotationDeg (0/90/180/270). dst must already have the rotated size.
void rotateInto(const PixelBuffer& src, int rotationDeg, PixelBuffer& dst);

// In-place helpers on whole buffers.
void scaleRgb256(PixelBuffer& b, std::uint32_t k);               // rgb only, alpha untouched (opaque dimming)
void mixInto(PixelBuffer& a, const PixelBuffer& b, std::uint32_t wB255);  // a = a*(255-w) + b*w (all channels)

}  // namespace ss::compose
