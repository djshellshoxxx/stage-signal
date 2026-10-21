#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include "doctest/doctest.h"
#include "ss/compose/compositor.hpp"
#include "ss/compose/font5x7.hpp"

namespace tu {
using namespace ss::compose;
using ss::msToNs;
using ss::secToNs;
using ss::Ns;

inline PixelBuffer solid(std::uint32_t w, std::uint32_t h, Px c) {
    PixelBuffer b(w, h);
    fill(b, c);
    return b;
}
inline std::shared_ptr<const PixelBuffer> sharedSolid(std::uint32_t w, std::uint32_t h, Px c) {
    return std::make_shared<const PixelBuffer>(solid(w, h, c));
}
inline Px at(const PixelBuffer& b, int x, int y) { return loadPx(b.row(std::uint32_t(y)) + std::size_t(x) * 4); }
inline void set(PixelBuffer& b, int x, int y, Px p) { storePx(b.row(std::uint32_t(y)) + std::size_t(x) * 4, p); }

inline std::uint64_t hash(const PixelBuffer& b) {  // FNV-1a
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t v : b.rgba) { h ^= v; h *= 1099511628211ull; }
    h ^= b.width; h *= 1099511628211ull; h ^= b.height; h *= 1099511628211ull;
    return h;
}
// Every pixel unique-ish: r = x*16, g = y*16, b = 7, a = 255
inline PixelBuffer coords(std::uint32_t w, std::uint32_t h) {
    PixelBuffer b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) set(b, int(x), int(y), Px{std::uint8_t(x * 16), std::uint8_t(y * 16), 7, 255});
    return b;
}
inline bool allEqual(const PixelBuffer& b, Px p) {
    for (std::uint32_t y = 0; y < b.height; ++y)
        for (std::uint32_t x = 0; x < b.width; ++x)
            if (at(b, int(x), int(y)) != p) return false;
    return true;
}
inline FrameInputs sceneOnly(const PixelBuffer* a) {
    FrameInputs in;
    in.scene.a = a;
    return in;
}
inline PixelBuffer copyOf(const ComposedFrame& f) { return *f.pixels; }

}  // namespace tu
