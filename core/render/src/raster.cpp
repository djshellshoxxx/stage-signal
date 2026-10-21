#include "ss/render/raster.hpp"
#include <cstdlib>
#include <utility>

namespace ss::render {
using namespace fx;

void fill(const Canvas& c, Rgb8 col) {
    if (!c.valid()) return;
    for (int y = 0; y < c.h; ++y) {
        std::uint8_t* p = c.at(0, y);
        for (int x = 0; x < c.w; ++x, p += 4) { p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = 255; }
    }
}

void fillRect(const Canvas& c, int x, int y, int w, int h, Rgb8 col, Blend m) {
    if (!c.valid() || w <= 0 || h <= 0) return;
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const long long xe = static_cast<long long>(x) + w, ye = static_cast<long long>(y) + h;
    const int x1 = xe > c.w ? c.w : static_cast<int>(xe), y1 = ye > c.h ? c.h : static_cast<int>(ye);
    for (int yy = y0; yy < y1; ++yy) {
        std::uint8_t* p = c.at(x0, yy);
        for (int xx = x0; xx < x1; ++xx, p += 4) {
            if (m == Blend::Add) {
                const int r = p[0] + col.r, g = p[1] + col.g, b = p[2] + col.b;
                p[0] = static_cast<std::uint8_t>(r > 255 ? 255 : r); p[1] = static_cast<std::uint8_t>(g > 255 ? 255 : g);
                p[2] = static_cast<std::uint8_t>(b > 255 ? 255 : b);
            } else { p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = 255; }
        }
    }
}

void gradientV(const Canvas& c, int x, int y, int w, int h, Rgb8 top, Rgb8 bottom) {
    for (int i = 0; i < h; ++i) {
        const int t = h > 1 ? (i * 256) / (h - 1) : 0;
        fillRect(c, x, y + i, w, 1, lerp(top, bottom, t));
    }
}
void gradientH(const Canvas& c, int x, int y, int w, int h, Rgb8 left, Rgb8 right) {
    for (int i = 0; i < w; ++i) {
        const int t = w > 1 ? (i * 256) / (w - 1) : 0;
        fillRect(c, x + i, y, 1, h, lerp(left, right, t));
    }
}

namespace {
void wuLine(const Canvas& c, std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1, Rgb8 col, Blend m, int dOff) {
    const bool steep = std::llabs(y1 - y0) > std::llabs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    const std::int64_t dx = x1 - x0, dy = y1 - y0;
    std::int64_t xs = (x0 + 128) >> 8, xe = (x1 + 128) >> 8;
    const std::int64_t lim = (steep ? c.h : c.w) - 1;
    if (xs < 0) xs = 0;
    if (xe > lim) xe = lim;
    if (xs > xe) return;
    const std::int64_t slope = dx ? (dy * 65536) / dx : 0;   // Q16, |slope| <= 1 after the steep swap
    for (std::int64_t x = xs; x <= xe; ++x) {
        const std::int64_t yq = (dx ? y0 + (((x * 256 - x0) * slope) >> 16) : y0) + std::int64_t(dOff) * 256;
        const std::int64_t yi = yq >> 8;
        if (yi < -1 || yi > 32768) continue;
        const int frac = static_cast<int>(yq & 255);
        const int c1 = 255 - frac, c2 = frac;
        if (steep) { putCover(c, static_cast<int>(yi), static_cast<int>(x), col, c1, m);
                     putCover(c, static_cast<int>(yi + 1), static_cast<int>(x), col, c2, m); }
        else       { putCover(c, static_cast<int>(x), static_cast<int>(yi), col, c1, m);
                     putCover(c, static_cast<int>(x), static_cast<int>(yi + 1), col, c2, m); }
    }
}
}  // namespace

void lineAA(const Canvas& c, int x0q, int y0q, int x1q, int y1q, Rgb8 col, Blend m, int thickness) {
    if (!c.valid()) return;
    if (thickness < 1) thickness = 1;
    if (thickness > 16) thickness = 16;
    for (int k = 0; k < thickness; ++k) wuLine(c, x0q, y0q, x1q, y1q, col, m, k - (thickness - 1) / 2);
}

void fillCircleAA(const Canvas& c, int cxq, int cyq, int rq, Rgb8 col, Blend m) {
    if (!c.valid() || rq <= 0) return;
    const int x0 = (cxq - rq - 256) >> 8, x1 = ((cxq + rq + 256) >> 8) + 1;
    const int y0 = (cyq - rq - 256) >> 8, y1 = ((cyq + rq + 256) >> 8) + 1;
    const std::int64_t inner = rq > 128 ? std::int64_t(rq - 128) * (rq - 128) : 0, outer = std::int64_t(rq + 128) * (rq + 128);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < c.h; ++y) {
        const std::int64_t dy = std::int64_t(y) * 256 - cyq;
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < c.w; ++x) {
            const std::int64_t dx = std::int64_t(x) * 256 - cxq, d2 = dx * dx + dy * dy;
            if (d2 >= outer) continue;
            int cov = 255;
            if (d2 > inner) cov = clampi(static_cast<int>(rq - std::int64_t(isqrt64(static_cast<std::uint64_t>(d2))) + 128), 0, 255);
            putCover(c, x, y, col, cov, m);
        }
    }
}

void ringAA(const Canvas& c, int cxq, int cyq, int rq, int thicknessq, Rgb8 col, Blend m) {
    if (!c.valid() || rq <= 0 || thicknessq <= 0) return;
    const int ro = rq + thicknessq / 2, ri = rq - thicknessq / 2;
    const int y0 = (cyq - ro - 256) >> 8, y1 = ((cyq + ro + 256) >> 8) + 1;
    const std::int64_t outer = std::int64_t(ro + 128) * (ro + 128);
    const std::int64_t inner = ri > 128 ? std::int64_t(ri - 128) * (ri - 128) : 0;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < c.h; ++y) {
        const std::int64_t dy = std::int64_t(y) * 256 - cyq, dy2 = dy * dy;
        if (dy2 >= outer) continue;
        // pixel span of the annulus on this row: [cx - xo, cx + xo] minus the hole (|dx| < xi)
        const int xo = static_cast<int>(isqrt64(static_cast<std::uint64_t>(outer - dy2)) >> 8) + 1;
        const int xi = dy2 < inner ? static_cast<int>(isqrt64(static_cast<std::uint64_t>(inner - dy2)) >> 8) - 1 : -1;
        const int cxp = cxq >> 8;
        int xa = cxp - xo, xb = cxp + xo;
        if (xa < 0) xa = 0;
        if (xb > c.w - 1) xb = c.w - 1;
        for (int x = xa; x <= xb; ++x) {
            if (xi > 0 && x > cxp - xi && x < cxp + xi) { x = cxp + xi - 1; continue; }
            const std::int64_t dx = std::int64_t(x) * 256 - cxq, d2 = dx * dx + dy2;
            if (d2 >= outer || d2 <= inner) continue;
            const int d = static_cast<int>(isqrt64(static_cast<std::uint64_t>(d2)));
            const int covO = clampi(ro - d + 128, 0, 256), covI = clampi(d - ri + 128, 0, 256);
            const int cov = (covO * covI) >> 8;
            putCover(c, x, y, col, cov > 255 ? 255 : cov, m);
        }
    }
}

void glowDisc(const Canvas& c, int cxq, int cyq, int rq, Rgb8 col, int intensityQ8) {
    if (!c.valid() || rq <= 0 || intensityQ8 <= 0) return;
    const int x0 = (cxq - rq) >> 8, x1 = ((cxq + rq) >> 8) + 1;
    const int y0 = (cyq - rq) >> 8, y1 = ((cyq + rq) >> 8) + 1;
    const std::int64_t r2 = std::int64_t(rq) * rq;
    const int inten = intensityQ8 > 256 ? 256 : intensityQ8;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < c.h; ++y) {
        const std::int64_t dy = std::int64_t(y) * 256 - cyq;
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < c.w; ++x) {
            const std::int64_t dx = std::int64_t(x) * 256 - cxq, d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            const int t = 256 - static_cast<int>((d2 * 256) / r2);   // 256 at centre, 0 at edge
            const int cov = (((t * t) >> 8) * inten) >> 8;
            putCover(c, x, y, col, cov > 255 ? 255 : cov, Blend::Add);
        }
    }
}

}  // namespace ss::render
