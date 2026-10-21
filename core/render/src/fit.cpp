#include "ss/render/fit.hpp"
#include <cstring>

namespace ss::render {

namespace {
int rdiv(std::int64_t a, std::int64_t b) { return b > 0 ? static_cast<int>((a + b / 2) / b) : 0; }
int normRot(int deg) { deg %= 360; if (deg < 0) deg += 360; return (deg == 90 || deg == 180 || deg == 270) ? deg : 0; }
}  // namespace

RectI fitRect(int srcW, int srcH, int dstW, int dstH, FitMode mode) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return {};
    if (mode == FitMode::Stretch) return {0, 0, dstW, dstH};
    const bool widthLimited = std::int64_t(srcW) * dstH >= std::int64_t(srcH) * dstW;
    const bool matchWidth = (mode == FitMode::Contain) ? widthLimited : !widthLimited;
    int w, h;
    if (matchWidth) { w = dstW; h = rdiv(std::int64_t(srcH) * dstW, srcW); if (h < 1) h = 1; }
    else            { h = dstH; w = rdiv(std::int64_t(srcW) * dstH, srcH); if (w < 1) w = 1; }
    if (mode == FitMode::Contain) { if (w > dstW) w = dstW; if (h > dstH) h = dstH; }
    return {(dstW - w) / 2, (dstH - h) / 2, w, h};
}

void rotatedSize(int w, int h, int rotationDeg, int& outW, int& outH) {
    const int r = normRot(rotationDeg);
    if (r == 90 || r == 270) { outW = h; outH = w; } else { outW = w; outH = h; }
}

RectI normToPixels(float nx, float ny, float nw, float nh, int w, int h) {
    auto edge = [](float n, int size) {
        if (!(n > 0.f)) return 0;
        if (n >= 1.f) return size;
        return static_cast<int>(n * static_cast<float>(size) + 0.5f);
    };
    const int x0 = edge(nx, w), y0 = edge(ny, h), x1 = edge(nx + nw, w), y1 = edge(ny + nh, h);
    return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

bool blitFit(const ss::PixelBuffer& src, ss::PixelBuffer& dst, FitMode mode, int rotationDeg) {
    if (src.empty() || dst.empty() || src.rgba.size() < std::size_t(src.width) * src.height * 4 ||
        dst.rgba.size() < std::size_t(dst.width) * dst.height * 4)
        return false;
    const int sw = static_cast<int>(src.width), sh = static_cast<int>(src.height);
    const int dw = static_cast<int>(dst.width), dh = static_cast<int>(dst.height);
    const int rot = normRot(rotationDeg);
    int rw, rh;
    rotatedSize(sw, sh, rot, rw, rh);
    const RectI r = fitRect(rw, rh, dw, dh, mode);
    for (int y = 0; y < dh; ++y) {
        std::uint8_t* o = dst.row(static_cast<std::uint32_t>(y));
        for (int x = 0; x < dw; ++x, o += 4) {
            const int lx = x - r.x, ly = y - r.y;
            if (lx < 0 || ly < 0 || lx >= r.w || ly >= r.h) { o[0] = o[1] = o[2] = 0; o[3] = 255; continue; }
            int u = static_cast<int>((std::int64_t(2 * lx + 1) * rw) / (2 * std::int64_t(r.w)));
            int v = static_cast<int>((std::int64_t(2 * ly + 1) * rh) / (2 * std::int64_t(r.h)));
            if (u >= rw) u = rw - 1;
            if (v >= rh) v = rh - 1;
            int sx, sy;
            switch (rot) {
                case 90:  sx = v;           sy = sh - 1 - u; break;
                case 180: sx = sw - 1 - u;  sy = sh - 1 - v; break;
                case 270: sx = sw - 1 - v;  sy = u;          break;
                default:  sx = u;           sy = v;          break;
            }
            std::memcpy(o, src.row(static_cast<std::uint32_t>(sy)) + std::size_t(sx) * 4, 4);
        }
    }
    return true;
}

}  // namespace ss::render
