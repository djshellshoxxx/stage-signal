// Small software raster toolbox for scenes (RGBA8 premultiplied, sRGB). Integer only, no allocation.
// Coordinates passed as "Q8" are pixel units * 256 where integer positions are PIXEL CENTRES.
// Canvases are assumed opaque (alpha 255) - scene frames always are - so "over" blending keeps alpha at 255.
#pragma once
#include <cstddef>
#include <cstdint>
#include "ss/base/types.hpp"
#include "ss/render/fixed.hpp"

namespace ss::render {

using fx::Rgb8;
enum class Blend : std::uint8_t { Over, Add };

struct Canvas {
    std::uint8_t* px = nullptr;
    int w = 0, h = 0;
    std::size_t stride = 0;
    Canvas() = default;
    // A buffer whose vector is smaller than width*height*4 yields an empty (draw-nothing) canvas.
    explicit Canvas(PixelBuffer& b) {
        if (b.width && b.height && b.width < 32768 && b.height < 32768 &&
            b.rgba.size() >= std::size_t(b.width) * b.height * 4) {
            px = b.rgba.data(); w = static_cast<int>(b.width); h = static_cast<int>(b.height); stride = b.stride();
        }
    }
    bool valid() const { return px != nullptr; }
    std::uint8_t* at(int x, int y) const { return px + std::size_t(y) * stride + std::size_t(x) * 4; }   // unchecked
};

// ---- pixel ops (bounds checked) ------------------------------------------------------------
inline void putAdd(const Canvas& c, int x, int y, Rgb8 s) {
    if (static_cast<unsigned>(x) >= static_cast<unsigned>(c.w) || static_cast<unsigned>(y) >= static_cast<unsigned>(c.h)) return;
    std::uint8_t* d = c.at(x, y);
    const int r = d[0] + s.r, g = d[1] + s.g, b = d[2] + s.b;
    d[0] = static_cast<std::uint8_t>(r > 255 ? 255 : r); d[1] = static_cast<std::uint8_t>(g > 255 ? 255 : g);
    d[2] = static_cast<std::uint8_t>(b > 255 ? 255 : b);
}
// Coverage 0..255 of colour `s`. Over: dst = s*cov + dst*(1-cov). Add: dst += s*cov.
inline void putCover(const Canvas& c, int x, int y, Rgb8 s, int cov, Blend m) {
    if (cov <= 0 || static_cast<unsigned>(x) >= static_cast<unsigned>(c.w) || static_cast<unsigned>(y) >= static_cast<unsigned>(c.h)) return;
    if (cov > 255) cov = 255;
    std::uint8_t* d = c.at(x, y);
    if (m == Blend::Add) {
        const int r = d[0] + fx::div255(s.r * cov), g = d[1] + fx::div255(s.g * cov), b = d[2] + fx::div255(s.b * cov);
        d[0] = static_cast<std::uint8_t>(r > 255 ? 255 : r); d[1] = static_cast<std::uint8_t>(g > 255 ? 255 : g);
        d[2] = static_cast<std::uint8_t>(b > 255 ? 255 : b);
    } else {
        const int inv = 255 - cov;
        d[0] = static_cast<std::uint8_t>(fx::div255(s.r * cov + d[0] * inv));
        d[1] = static_cast<std::uint8_t>(fx::div255(s.g * cov + d[1] * inv));
        d[2] = static_cast<std::uint8_t>(fx::div255(s.b * cov + d[2] * inv));
    }
}
inline void putOpaque(const Canvas& c, int x, int y, Rgb8 s) {
    if (static_cast<unsigned>(x) >= static_cast<unsigned>(c.w) || static_cast<unsigned>(y) >= static_cast<unsigned>(c.h)) return;
    std::uint8_t* d = c.at(x, y);
    d[0] = s.r; d[1] = s.g; d[2] = s.b; d[3] = 255;
}

// ---- shapes (all clipped to the canvas) ----------------------------------------------------
void fill(const Canvas& c, Rgb8 colour);                                               // opaque solid fill
void fillRect(const Canvas& c, int x, int y, int w, int h, Rgb8 colour, Blend m = Blend::Over);   // Over = opaque write
void gradientV(const Canvas& c, int x, int y, int w, int h, Rgb8 top, Rgb8 bottom);    // opaque
void gradientH(const Canvas& c, int x, int y, int w, int h, Rgb8 left, Rgb8 right);    // opaque
// Anti-aliased (Wu) line, 1 px wide, endpoints Q8. thickness>1 draws parallel copies (whole pixels).
void lineAA(const Canvas& c, int x0q, int y0q, int x1q, int y1q, Rgb8 colour, Blend m = Blend::Add, int thickness = 1);
// Anti-aliased filled disc / ring. Centre and radius Q8.
void fillCircleAA(const Canvas& c, int cxq, int cyq, int rq, Rgb8 colour, Blend m = Blend::Over);
void ringAA(const Canvas& c, int cxq, int cyq, int rq, int thicknessq, Rgb8 colour, Blend m = Blend::Add);
// Soft additive glow: intensity (Q8, 0..256) at the centre falling off quadratically to zero at radius rq.
void glowDisc(const Canvas& c, int cxq, int cyq, int rq, Rgb8 colour, int intensityQ8);

}  // namespace ss::render
