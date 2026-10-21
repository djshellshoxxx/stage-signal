// PrismMirror (kaleidoscope fold) and BloomPulse (audio-reactive additive bloom). Integer maths only.
#include "ss/render/effects.hpp"
#include <cstring>
#include "ss/render/fixed.hpp"

namespace ss::render {
using namespace fx;

void EffectScratch::prepare(std::uint32_t w, std::uint32_t h) {
    // Bounds first, allocation second (limit: 4096 x 4096, far above the 400x300 BASE internal frame).
    if (w == 0 || h == 0 || w > 4096 || h > 4096) { w_ = h_ = lw_ = lh_ = 0; return; }
    w_ = w; h_ = h;
    lw_ = (w + 3) / 4; lh_ = (h + 3) / 4;
    const std::size_t n = std::size_t(w) * h;
    copy.assign(n * 4, 0);
    ang.assign(n, 0);
    rad.assign(n, 0);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x, ++i) {
            const std::int32_t dx2 = static_cast<std::int32_t>(2 * x + 1) - static_cast<std::int32_t>(w);
            const std::int32_t dy2 = static_cast<std::int32_t>(2 * y + 1) - static_cast<std::int32_t>(h);
            ang[i] = static_cast<std::uint16_t>(atan2Q16(dy2, dx2));
            const std::uint32_t r = isqrt64(std::uint64_t(std::int64_t(dx2) * dx2 + std::int64_t(dy2) * dy2));
            rad[i] = static_cast<std::uint16_t>(r > 65535 ? 65535 : r);   // half pixels
        }
    sinTab.assign(1024, 0);
    for (int k = 0; k < 1024; ++k) sinTab[std::size_t(k)] = static_cast<std::int16_t>(sinQ15(static_cast<std::uint32_t>(k) << 6));
    const std::size_t ln = std::size_t(lw_) * lh_ * 3;
    bloomA.assign(ln, 0);
    bloomB.assign(ln, 0);
    colIdx.assign(w, 0); colFrac.assign(w, 0); rowIdx.assign(h, 0); rowFrac.assign(h, 0);
    for (std::uint32_t x = 0; x < w; ++x) {
        int pos = static_cast<int>((std::int64_t(2 * x + 1) * lw_ * 128) / w) - 128;
        if (pos < 0) pos = 0;
        int i0 = pos >> 8;
        if (i0 > static_cast<int>(lw_) - 1) i0 = static_cast<int>(lw_) - 1;
        colIdx[x] = static_cast<std::uint16_t>(i0); colFrac[x] = static_cast<std::uint16_t>(pos & 255);
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        int pos = static_cast<int>((std::int64_t(2 * y + 1) * lh_ * 128) / h) - 128;
        if (pos < 0) pos = 0;
        int i0 = pos >> 8;
        if (i0 > static_cast<int>(lh_) - 1) i0 = static_cast<int>(lh_) - 1;
        rowIdx[y] = static_cast<std::uint16_t>(i0); rowFrac[y] = static_cast<std::uint16_t>(pos & 255);
    }
}

std::size_t EffectScratch::bytes() const {
    return copy.size() + (ang.size() + rad.size() + bloomA.size() + bloomB.size() + colIdx.size() + colFrac.size() + rowIdx.size() + rowFrac.size()) * 2 +
           sinTab.size() * 2;
}

namespace {

bool frameOk(const PixelBuffer& f, const EffectScratch& s) {
    return s.ready(f.width, f.height) && f.rgba.size() >= std::size_t(f.width) * f.height * 4;
}

void prismMirror(int amountQ8, const SceneInput& in, PixelBuffer& f, EffectScratch& s) {
    static const model::SceneParams kDef{};
    const model::SceneParams& p = in.params ? *in.params : kDef;
    const int n = clampi(p.prismSegments, 2, 16);
    const int speedQ8 = toQ8(p.speed, 8.f);
    const int w = static_cast<int>(f.width), h = static_cast<int>(f.height);
    std::memcpy(s.copy.data(), f.rgba.data(), std::size_t(w) * h * 4);
    const std::uint32_t rot = phaseQ16(in.timeNs, speedQ8, 40000);   // one turn per 40 s at speed 1
    const std::uint32_t base = rot >> 1;                             // the sampled wedge sweeps the source
    const std::uint32_t invN = 65536u / static_cast<std::uint32_t>(n), un = static_cast<std::uint32_t>(n);
    const std::int16_t* tab = s.sinTab.data();
    const std::uint8_t* src = s.copy.data();
    std::uint8_t* dst = f.rgba.data();
    std::size_t i = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x, ++i) {
            const std::uint32_t t = ((std::uint32_t(s.ang[i]) + rot) & 0xFFFFu) * un;
            const std::uint32_t frac = t & 0xFFFFu;
            const std::uint32_t fold = frac < 32768u ? frac : 65536u - frac;      // mirrored position inside the wedge
            const std::uint32_t sa = ((fold * invN) >> 16) + base;
            const std::int64_t r = s.rad[i];
            const int cs = tab[((sa >> 6) + 256u) & 1023u], sn = tab[(sa >> 6) & 1023u];
            int sx = (w - 1 + static_cast<int>((r * cs) >> 15)) >> 1, sy = (h - 1 + static_cast<int>((r * sn) >> 15)) >> 1;
            sx = clampi(sx, 0, w - 1); sy = clampi(sy, 0, h - 1);
            const std::uint8_t* m = src + (std::size_t(sy) * w + sx) * 4;
            const std::uint8_t* o = src + i * 4;
            std::uint8_t* d = dst + i * 4;
            if (amountQ8 >= 256) { d[0] = m[0]; d[1] = m[1]; d[2] = m[2]; d[3] = m[3]; }
            else {
                d[0] = static_cast<std::uint8_t>(o[0] + (((m[0] - o[0]) * amountQ8) >> 8));
                d[1] = static_cast<std::uint8_t>(o[1] + (((m[1] - o[1]) * amountQ8) >> 8));
                d[2] = static_cast<std::uint8_t>(o[2] + (((m[2] - o[2]) * amountQ8) >> 8));
                d[3] = static_cast<std::uint8_t>(o[3] + (((m[3] - o[3]) * amountQ8) >> 8));
            }
        }
}

void blur5(const std::uint16_t* in, std::uint16_t* out, int lw, int lh, bool horizontal) {
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x)
            for (int c = 0; c < 3; ++c) {
                int sum = 0;
                for (int k = -2; k <= 2; ++k) {
                    const int xx = horizontal ? clampi(x + k, 0, lw - 1) : x;
                    const int yy = horizontal ? y : clampi(y + k, 0, lh - 1);
                    sum += in[(std::size_t(yy) * lw + xx) * 3 + c];
                }
                out[(std::size_t(y) * lw + x) * 3 + c] = static_cast<std::uint16_t>((sum * 205) >> 10);
            }
}

void bloomPulse(int amountQ8, const SceneInput& in, PixelBuffer& f, EffectScratch& s) {
    const int w = static_cast<int>(f.width), h = static_cast<int>(f.height);
    const int lw = static_cast<int>(s.lw_), lh = static_cast<int>(s.lh_);
    const std::uint8_t* px = f.rgba.data();
    std::uint16_t* A = s.bloomA.data();
    std::uint16_t* B = s.bloomB.data();

    // 1. 4x4 box downsample + bright pass (threshold 96, rescaled to 0..255)
    for (int ly = 0; ly < lh; ++ly)
        for (int lx = 0; lx < lw; ++lx) {
            int sr = 0, sg = 0, sb = 0, cnt = 0;
            const int y1 = (ly * 4 + 4) < h ? ly * 4 + 4 : h, x1 = (lx * 4 + 4) < w ? lx * 4 + 4 : w;
            for (int y = ly * 4; y < y1; ++y) {
                const std::uint8_t* p = px + (std::size_t(y) * w + lx * 4) * 4;
                for (int x = lx * 4; x < x1; ++x, p += 4) { sr += p[0]; sg += p[1]; sb += p[2]; ++cnt; }
            }
            const int ch[3] = {sr / cnt, sg / cnt, sb / cnt};
            for (int c = 0; c < 3; ++c) {
                int v = ch[c] > 96 ? ((ch[c] - 96) * 410) >> 8 : 0;
                A[(std::size_t(ly) * lw + lx) * 3 + c] = static_cast<std::uint16_t>(v > 255 ? 255 : v);
            }
        }

    // 2. two box-blur iterations (separable, 5 taps) at 1/4 resolution
    blur5(A, B, lw, lh, true);  blur5(B, A, lw, lh, false);
    blur5(A, B, lw, lh, true);  blur5(B, A, lw, lh, false);

    // 3. strength follows low band + onset. Strobing disabled narrows the pulse range so it cannot flash.
    int low = 0, onset = 0;
    if (in.features) {
        low = toQ8(in.features->band[0]);
        onset = toQ8(in.features->onsetStrength);
        if (in.features->onsetFired) onset = 256;
    }
    int pulse = 64 + ((low * 100) >> 8) + ((onset * 100) >> 8);
    pulse = in.strobingDisabled ? clampi(pulse, 64, 112) : clampi(pulse, 0, 256);
    const int gain = ((amountQ8 * pulse) >> 8) * 2;     // up to 2x: glow may exceed the source it came from

    // 4. bilinear upsample + premultiplied-safe additive composite (colour never exceeds alpha)
    std::uint8_t* dst = f.rgba.data();
    for (int y = 0; y < h; ++y) {
        const int y0 = s.rowIdx[std::size_t(y)], y1 = y0 + 1 < lh ? y0 + 1 : y0, fy = s.rowFrac[std::size_t(y)];
        std::uint8_t* d = dst + std::size_t(y) * w * 4;
        for (int x = 0; x < w; ++x, d += 4) {
            const int x0 = s.colIdx[std::size_t(x)], x1 = x0 + 1 < lw ? x0 + 1 : x0, fx2 = s.colFrac[std::size_t(x)];
            const std::uint16_t* p00 = A + (std::size_t(y0) * lw + x0) * 3;
            const std::uint16_t* p01 = A + (std::size_t(y0) * lw + x1) * 3;
            const std::uint16_t* p10 = A + (std::size_t(y1) * lw + x0) * 3;
            const std::uint16_t* p11 = A + (std::size_t(y1) * lw + x1) * 3;
            for (int c = 0; c < 3; ++c) {
                const int top = (p00[c] * (256 - fx2) + p01[c] * fx2) >> 8, bot = (p10[c] * (256 - fx2) + p11[c] * fx2) >> 8;
                const int g = (top * (256 - fy) + bot * fy) >> 8;
                int v = d[c] + ((g * gain) >> 8);
                if (v > d[3]) v = d[3];
                if (v < d[c]) v = d[c];       // never darken (covers colour already above alpha)
                d[c] = static_cast<std::uint8_t>(v);
            }
        }
    }
}

}  // namespace

void applyEffect(model::EffectKind kind, float amount, const SceneInput& in, PixelBuffer& inOut, EffectScratch& scratch) {
    const int amountQ8 = toQ8(amount);
    if (amountQ8 == 0 || kind == model::EffectKind::None || !frameOk(inOut, scratch)) return;
    switch (kind) {
        case model::EffectKind::PrismMirror: prismMirror(amountQ8, in, inOut, scratch); break;
        case model::EffectKind::BloomPulse: bloomPulse(amountQ8, in, inOut, scratch); break;
        default: break;
    }
}

}  // namespace ss::render
