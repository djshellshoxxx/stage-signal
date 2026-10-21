// Pixel conventions for compose.
//  * RGBA8, PREMULTIPLIED alpha (colour channels already multiplied by alpha), sRGB, SDR, tightly packed rows.
//  * Invariant for valid input: c <= a for r,g,b. Operators saturate at 255 so invalid input cannot overflow.
//  * All blending is integer fixed point with div255() rounding => bit-identical on every platform.
#pragma once
#include <algorithm>
#include <cstdint>
#include "ss/base/types.hpp"
#include "ss/model/show_model.hpp"

namespace ss::compose {

using Rgb = ss::model::Rgb;
constexpr std::uint32_t div255(std::uint32_t x) {  // round(x/255) for x <= 65025*2
    x += 128;
    return (x + (x >> 8)) >> 8;
}
constexpr std::uint8_t sat8(std::uint32_t v) { return static_cast<std::uint8_t>(v > 255 ? 255 : v); }

struct Px { std::uint8_t r = 0, g = 0, b = 0, a = 0; };
inline Px loadPx(const std::uint8_t* p) { return Px{p[0], p[1], p[2], p[3]}; }
inline void storePx(std::uint8_t* p, Px v) { p[0] = v.r; p[1] = v.g; p[2] = v.b; p[3] = v.a; }
constexpr bool operator==(Px a, Px b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
constexpr bool operator!=(Px a, Px b) { return !(a == b); }

// Source-over, premultiplied: d' = s + d * (1 - sa).
inline Px over(Px s, Px d) {
    const std::uint32_t inv = 255u - s.a;
    return Px{sat8(s.r + div255(d.r * inv)), sat8(s.g + div255(d.g * inv)), sat8(s.b + div255(d.b * inv)),
              sat8(s.a + div255(d.a * inv))};
}
// Scale all four channels by k/256 (k in 0..256), rounding. k==256 is the identity.
inline Px scale256(Px p, std::uint32_t k) {
    return Px{static_cast<std::uint8_t>((p.r * k + 128) >> 8), static_cast<std::uint8_t>((p.g * k + 128) >> 8),
              static_cast<std::uint8_t>((p.b * k + 128) >> 8), static_cast<std::uint8_t>((p.a * k + 128) >> 8)};
}
// Premultiplied opaque colour from a straight Rgb.
inline Px opaque(Rgb c) { return Px{c.r, c.g, c.b, 255}; }
inline Px premul(Rgb c, std::uint32_t alpha255) {
    return Px{static_cast<std::uint8_t>(div255(c.r * alpha255)), static_cast<std::uint8_t>(div255(c.g * alpha255)),
              static_cast<std::uint8_t>(div255(c.b * alpha255)), static_cast<std::uint8_t>(alpha255)};
}
inline std::uint32_t luma8(Px p) { return (54u * p.r + 183u * p.g + 19u * p.b + 128u) >> 8; }  // Rec.709 weights

using ss::PixelBuffer;

struct Rect {  // pixel rectangle
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
struct RectF {  // normalised rectangle, 0..1 of the canvas (origin top-left)
    float x = 0.f, y = 0.f, w = 1.f, h = 1.f;
};

enum class FitMode : std::uint8_t { Fit, Crop, Stretch };  // letterbox | cover | distort
enum class Sampling : std::uint8_t { Nearest, Bilinear };

}  // namespace ss::compose
