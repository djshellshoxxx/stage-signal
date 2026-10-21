#include "ss/compose/raster.hpp"
#include <cmath>
#include <cstring>
#include "ss/compose/limits.hpp"

namespace ss::compose {

bool validDims(std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0 || w > Limits::kMaxDimension || h > Limits::kMaxDimension) return false;
    return std::uint64_t(w) * h * 4 <= Limits::kMaxBufferBytes;
}

void resizeKeep(PixelBuffer& b, std::uint32_t w, std::uint32_t h) {
    b.width = w;
    b.height = h;
    b.rgba.resize(std::size_t(w) * h * 4);
}

void fill(PixelBuffer& dst, Px c) {
    const std::size_t n = std::size_t(dst.width) * dst.height;
    std::uint8_t* p = dst.rgba.data();
    for (std::size_t i = 0; i < n; ++i, p += 4) storePx(p, c);
}

void fillRect(PixelBuffer& dst, Rect r, Px c, bool blendOver) {
    const int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    const int x1 = std::min(r.x + r.w, int(dst.width)), y1 = std::min(r.y + r.h, int(dst.height));
    for (int y = y0; y < y1; ++y) {
        std::uint8_t* p = dst.row(std::uint32_t(y)) + std::size_t(x0) * 4;
        for (int x = x0; x < x1; ++x, p += 4) storePx(p, blendOver ? over(c, loadPx(p)) : c);
    }
}

Rect toPixelRect(RectF r, int cw, int ch) {
    auto clamp01 = [](double v) { return v != v ? 0.0 : (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)); };
    const double x0 = clamp01(r.x), y0 = clamp01(r.y);
    const double x1 = clamp01(double(r.x) + double(r.w)), y1 = clamp01(double(r.y) + double(r.h));
    int px0 = int(std::lround(x0 * cw)), py0 = int(std::lround(y0 * ch));
    int px1 = int(std::lround(x1 * cw)), py1 = int(std::lround(y1 * ch));
    if (px1 <= px0) { px1 = std::min(cw, px0 + 1); px0 = std::max(0, px1 - 1); }
    if (py1 <= py0) { py1 = std::min(ch, py0 + 1); py0 = std::max(0, py1 - 1); }
    return Rect{px0, py0, px1 - px0, py1 - py0};
}

FitResult computeFit(int sw, int sh, Rect region, FitMode mode) {
    FitResult out;
    if (sw <= 0 || sh <= 0 || region.empty()) return out;
    const std::int64_t dw = region.w, dh = region.h;
    if (mode == FitMode::Stretch) {
        out.src = Rect{0, 0, sw, sh};
        out.dst = region;
    } else if (mode == FitMode::Fit) {
        out.src = Rect{0, 0, sw, sh};
        std::int64_t w, h;
        if (dw * sh <= dh * sw) {  // width limited
            w = dw;
            h = (std::int64_t(sh) * dw + sw / 2) / sw;
        } else {
            h = dh;
            w = (std::int64_t(sw) * dh + sh / 2) / sh;
        }
        w = std::max<std::int64_t>(1, std::min(w, dw));
        h = std::max<std::int64_t>(1, std::min(h, dh));
        out.dst = Rect{region.x + int((dw - w) / 2), region.y + int((dh - h) / 2), int(w), int(h)};
    } else {  // Crop
        out.dst = region;
        if (dw * sh > dh * std::int64_t(sw)) {  // region wider than source: keep full width
            std::int64_t h = (std::int64_t(sw) * dh + dw / 2) / dw;
            h = std::max<std::int64_t>(1, std::min<std::int64_t>(h, sh));
            out.src = Rect{0, int((sh - h) / 2), sw, int(h)};
        } else {
            std::int64_t w = (std::int64_t(sh) * dw + dh / 2) / dh;
            w = std::max<std::int64_t>(1, std::min<std::int64_t>(w, sw));
            out.src = Rect{int((sw - w) / 2), 0, int(w), sh};
        }
    }
    return out;
}

namespace {
// Position (24.8 fixed) of destination index i inside a source span of srcLen pixels, corner aligned.
inline std::uint32_t pos256(std::uint32_t i, std::uint32_t dstLen, std::uint32_t srcLen) {
    if (dstLen <= 1 || srcLen <= 1) return 0;
    return static_cast<std::uint32_t>((std::uint64_t(i) * (srcLen - 1) * 256ull) / (dstLen - 1));
}
inline Px lerpPx(Px a, Px b, std::uint32_t f) {  // f 0..255
    const std::uint32_t g = 256 - f;
    return Px{static_cast<std::uint8_t>((a.r * g + b.r * f + 128) >> 8), static_cast<std::uint8_t>((a.g * g + b.g * f + 128) >> 8),
              static_cast<std::uint8_t>((a.b * g + b.b * f + 128) >> 8), static_cast<std::uint8_t>((a.a * g + b.a * f + 128) >> 8)};
}
}  // namespace

void drawScaled(const PixelBuffer& src, Rect sr, PixelBuffer& dst, Rect dr, const DrawParams& p) {
    if (src.empty() || dst.empty() || dr.empty() || sr.w <= 0 || sr.h <= 0) return;
    if (sr.x < 0 || sr.y < 0 || sr.x + sr.w > int(src.width) || sr.y + sr.h > int(src.height)) return;
    const int cx0 = std::max(dr.x, 0), cy0 = std::max(dr.y, 0);
    const int cx1 = std::min(dr.x + dr.w, int(dst.width)), cy1 = std::min(dr.y + dr.h, int(dst.height));
    if (cx0 >= cx1 || cy0 >= cy1) return;
    const std::uint32_t sw = std::uint32_t(sr.w), sh = std::uint32_t(sr.h);
    const std::uint32_t dw = std::uint32_t(dr.w), dh = std::uint32_t(dr.h);
    const bool bil = p.sampling == Sampling::Bilinear;
    for (int y = cy0; y < cy1; ++y) {
        const std::uint32_t py = pos256(std::uint32_t(y - dr.y), dh, sh);
        std::uint32_t y0 = py >> 8;
        const std::uint32_t fy = py & 255u;
        const std::uint32_t y1 = std::min(y0 + 1, sh - 1);
        if (!bil) y0 = std::min((py + 128) >> 8, sh - 1);
        const std::uint8_t* row0 = src.row(std::uint32_t(sr.y) + y0);
        const std::uint8_t* row1 = src.row(std::uint32_t(sr.y) + y1);
        std::uint8_t* out = dst.row(std::uint32_t(y)) + std::size_t(cx0) * 4;
        for (int x = cx0; x < cx1; ++x, out += 4) {
            std::uint32_t ix = std::uint32_t(x - dr.x);
            if (p.flipX) ix = dw - 1 - ix;
            const std::uint32_t px = pos256(ix, dw, sw);
            Px s;
            if (bil) {
                const std::uint32_t x0 = px >> 8, fx = px & 255u, x1 = std::min(x0 + 1, sw - 1);
                const std::size_t o0 = (std::size_t(sr.x) + x0) * 4, o1 = (std::size_t(sr.x) + x1) * 4;
                const Px top = lerpPx(loadPx(row0 + o0), loadPx(row0 + o1), fx);
                const Px bot = lerpPx(loadPx(row1 + o0), loadPx(row1 + o1), fx);
                s = lerpPx(top, bot, fy);
            } else {
                const std::uint32_t x0 = std::min((px + 128) >> 8, sw - 1);
                s = loadPx(row0 + (std::size_t(sr.x) + x0) * 4);
            }
            if (p.maskTint) s = premul(p.tint, s.a);
            if (p.opacity256 != 256) s = scale256(s, p.opacity256);
            storePx(out, p.blendOver ? over(s, loadPx(out)) : s);
        }
    }
}

void drawFitted(const PixelBuffer& src, RectF crop, PixelBuffer& dst, Rect region, FitMode mode, const DrawParams& p) {
    if (src.empty() || region.empty()) return;
    const Rect full{0, 0, int(src.width), int(src.height)};
    Rect window = full;
    if (crop.x != 0.f || crop.y != 0.f || crop.w != 1.f || crop.h != 1.f) window = toPixelRect(crop, full.w, full.h);
    FitResult f = computeFit(window.w, window.h, region, mode);
    if (f.dst.empty()) return;
    f.src.x += window.x;
    f.src.y += window.y;
    drawScaled(src, f.src, dst, f.dst, p);
}

void rotateInto(const PixelBuffer& src, int deg, PixelBuffer& dst) {
    const std::uint32_t w = src.width, h = src.height;
    if (deg == 0) {
        if (dst.rgba.size() == src.rgba.size() && !src.rgba.empty())
            std::memcpy(dst.rgba.data(), src.rgba.data(), src.rgba.size());
        return;
    }
    const bool swap = deg == 90 || deg == 270;
    if (swap ? (dst.width != h || dst.height != w) : (dst.width != w || dst.height != h)) return;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* s = src.row(y);
        for (std::uint32_t x = 0; x < w; ++x, s += 4) {
            std::uint32_t nx, ny;
            if (deg == 90) { nx = h - 1 - y; ny = x; }
            else if (deg == 180) { nx = w - 1 - x; ny = h - 1 - y; }
            else { nx = y; ny = w - 1 - x; }
            std::memcpy(dst.row(ny) + std::size_t(nx) * 4, s, 4);
        }
    }
}

void scaleRgb256(PixelBuffer& b, std::uint32_t k) {
    if (k >= 256) return;
    const std::size_t n = std::size_t(b.width) * b.height;
    std::uint8_t* p = b.rgba.data();
    for (std::size_t i = 0; i < n; ++i, p += 4) {
        p[0] = static_cast<std::uint8_t>((p[0] * k + 128) >> 8);
        p[1] = static_cast<std::uint8_t>((p[1] * k + 128) >> 8);
        p[2] = static_cast<std::uint8_t>((p[2] * k + 128) >> 8);
    }
}

void mixInto(PixelBuffer& a, const PixelBuffer& b, std::uint32_t w) {
    if (a.width != b.width || a.height != b.height) return;
    const std::uint32_t wa = 255u - w;
    const std::size_t n = a.rgba.size();
    std::uint8_t* pa = a.rgba.data();
    const std::uint8_t* pb = b.rgba.data();
    for (std::size_t i = 0; i < n; ++i) pa[i] = static_cast<std::uint8_t>(div255(pa[i] * wa + pb[i] * w));
}

}  // namespace ss::compose
