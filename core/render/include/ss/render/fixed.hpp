// Integer/fixed-point helpers shared by scenes, effects and raster code. Everything here is integer math
// (float -> int conversions only multiply by powers of two) so output is bit-identical on gcc/clang/MSVC.
#pragma once
#include <cmath>
#include <cstdint>
#include "ss/base/time.hpp"

namespace ss::render::fx {

struct Rgb8 { std::uint8_t r = 0, g = 0, b = 0; };

constexpr int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr int div255(int x) { return (x + 128 + ((x + 128) >> 8)) >> 8; }   // exact round(x/255) for 0..65025

// float -> Q8 with clamp to [0, maxValue]. Scaling by 256 is exact in binary floating point.
inline int toQ8(float v, float maxValue = 1.f) {
    if (!(v > 0.f)) return 0;
    if (v >= maxValue) return static_cast<int>(maxValue * 256.f);
    return static_cast<int>(v * 256.f);
}
// float turns (any range) -> Q16 turn fraction 0..65535
inline std::uint32_t toTurnsQ16(float v) {
    if (!(v > -1000.f && v < 1000.f)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v * 65536.f)) & 0xFFFFu;
}

// Phase in turns (Q16) after `timeNs` at `speedQ8` for an oscillation of `periodMs` at speed 1.
// Derived purely from elapsed time, so skipped frames never change motion speed.
inline std::uint32_t phaseQ16(Ns timeNs, int speedQ8, std::uint32_t periodMs) {
    const std::uint64_t tUs = timeNs > 0 ? static_cast<std::uint64_t>(timeNs / 1000) : 0;
    const std::uint64_t num = tUs * static_cast<std::uint64_t>(speedQ8 < 0 ? 0 : speedQ8);
    const std::uint64_t den = static_cast<std::uint64_t>(periodMs ? periodMs : 1) * 1000ull * 256ull;
    const std::uint64_t r = num % den;
    return static_cast<std::uint32_t>((r << 16) / den);
}

// Sine of a Q16 turn phase, result Q15 (-32767..32767). Parabola + one refinement step, integer only.
inline int sinQ15(std::uint32_t phase) {
    const std::uint32_t p = phase & 0xFFFFu;
    const bool neg = p >= 0x8000u;
    const std::int64_t t = static_cast<std::int64_t>((p & 0x7FFFu) * 2u);          // Q16 position in half cycle
    std::int64_t y = (t * (65536 - t)) >> 14;                                       // 4t(1-t), Q16, peak 65536
    y = y + ((225 * (((y * y) >> 16) - y)) / 1000);                                 // refine (0.225)
    int s = static_cast<int>(y >> 1);
    if (s > 32767) s = 32767;
    return neg ? -s : s;
}
inline int cosQ15(std::uint32_t phase) { return sinQ15(phase + 0x4000u); }

// Exact floor(sqrt(v)). The hardware sqrt only seeds the estimate; integer correction makes the result exact and
// therefore identical on every compiler/FPU regardless of floating point flags.
inline std::uint32_t isqrt64(std::uint64_t v) {
    std::uint64_t r = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(v)));
    if (r > 0xFFFFFFFFull) r = 0xFFFFFFFFull;
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v && r < 0xFFFFFFFFull) ++r;
    return static_cast<std::uint32_t>(r);
}

// atan2 in turns Q16 [0, 65535]; max error ~0.2 degrees. (0,0) -> 0.
inline std::uint32_t atan2Q16(std::int32_t y, std::int32_t x) {
    if (x == 0 && y == 0) return 0;
    const std::int64_t ax = x < 0 ? -static_cast<std::int64_t>(x) : x;
    const std::int64_t ay = y < 0 ? -static_cast<std::int64_t>(y) : y;
    const bool swap = ay > ax;
    const std::int64_t mn = swap ? ax : ay, mx = swap ? ay : ax;
    const std::int64_t r = (mn << 15) / mx;                                         // Q15, 0..32768
    std::int64_t a = ((8192 * r) >> 15) + ((2847 * ((r * (32768 - r)) >> 15)) >> 15);   // octant angle, Q16 turns
    if (swap) a = 16384 - a;
    if (x < 0) a = 32768 - a;
    if (y < 0) a = 65536 - a;
    return static_cast<std::uint32_t>(a) & 0xFFFFu;
}

// HSV -> RGB. h Q16 turns (wraps), s and v Q8 (0..256).
inline Rgb8 hsv(std::uint32_t h, int sQ8, int vQ8) {
    const int s = clampi(sQ8, 0, 256), v = clampi(vQ8, 0, 255);
    const std::uint32_t hh = (h & 0xFFFFu) * 6u;
    const int region = static_cast<int>(hh >> 16), f = static_cast<int>(hh & 0xFFFFu);
    const int p = (v * (256 - s)) >> 8;
    const int q = (v * (256 - ((s * f) >> 16))) >> 8;
    const int t = (v * (256 - ((s * (65536 - f)) >> 16))) >> 8;
    int r, g, b;
    switch (region) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return {static_cast<std::uint8_t>(clampi(r, 0, 255)), static_cast<std::uint8_t>(clampi(g, 0, 255)),
            static_cast<std::uint8_t>(clampi(b, 0, 255))};
}

inline Rgb8 scale(Rgb8 c, int q8) {   // multiply by Q8 factor (0..256+), saturating
    return {static_cast<std::uint8_t>(clampi((c.r * q8) >> 8, 0, 255)), static_cast<std::uint8_t>(clampi((c.g * q8) >> 8, 0, 255)),
            static_cast<std::uint8_t>(clampi((c.b * q8) >> 8, 0, 255))};
}
inline Rgb8 lerp(Rgb8 a, Rgb8 b, int tQ8) {
    const int t = clampi(tQ8, 0, 256);
    return {static_cast<std::uint8_t>(a.r + (((b.r - a.r) * t) >> 8)), static_cast<std::uint8_t>(a.g + (((b.g - a.g) * t) >> 8)),
            static_cast<std::uint8_t>(a.b + (((b.b - a.b) * t) >> 8))};
}

}  // namespace ss::render::fx
