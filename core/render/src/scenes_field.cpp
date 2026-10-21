// Tunnel, ColorField, Starburst, PlasmaLite: full-frame per-pixel scenes (integer LUT driven).
#include "scenes.hpp"

namespace ss::render::detail {
using namespace fx;

namespace {

class Tunnel final : public ProcScene {
public:
    const char* name() const override { return "Tunnel"; }
    float costEstimate() const override { return 2.0f; }
    bool usesStrobe() const override { return false; }
protected:
    void allocate() override {
        lut_.build(w_, h_);
        vlut_.assign(lut_.rad.size(), 0);
        for (std::size_t i = 0; i < vlut_.size(); ++i) {
            const std::uint32_t r = lut_.rad[i] < 8 ? 8u : lut_.rad[i];
            vlut_[i] = (std::uint32_t(lut_.halfMin16) * 256u) / r;       // 256 at the rim, grows toward the centre
        }
    }
    void initState(ss::Rng& rng) override { seg_ = 6 + 2 * static_cast<int>(rng.below(4)); }
    void draw(const Canvas& cv, const Drive& d) override {
        Palette256 pal;
        pal.build(d, 0, 49152u, 256);
        const std::uint32_t tOff = d.phase(1800) >> 8;                      // 0..255 = two ring flips, seamless
        const std::uint32_t rot = d.phase(12000);
        const std::uint32_t spiral = static_cast<std::uint32_t>(6 + (d.intensQ8 >> 4));
        const int dark = 70 + (d.low >> 2);
        for (int y = 0; y < h_; ++y) {
            std::uint8_t* o = cv.at(0, y);
            const std::size_t base = std::size_t(y) * w_;
            for (int x = 0; x < w_; ++x, o += 4) {
                const std::uint32_t v = vlut_[base + x];
                const std::uint32_t a2 = (lut_.ang[base + x] + rot + v * spiral) & 0xFFFFu;
                const std::uint32_t a = (a2 * std::uint32_t(seg_)) >> 16, b = (v + tOff) >> 7;
                int val = ((a ^ b) & 1u) ? 256 : dark;
                const int shade = clampi(static_cast<int>(lut_.radN[base + x]) * 3 - 20, 0, 256);
                val = (val * shade) >> 8;
                const Rgb8 c = pal.c[(b * 9u + a * 5u) & 255u];
                o[0] = static_cast<std::uint8_t>((c.r * val) >> 8);
                o[1] = static_cast<std::uint8_t>((c.g * val) >> 8);
                o[2] = static_cast<std::uint8_t>((c.b * val) >> 8);
            }
        }
    }
private:
    PolarLut lut_;
    std::vector<std::uint32_t> vlut_;
    int seg_ = 8;
};

// Four-corner bilinear colour wash. Flashes (full-frame lift) on onset events unless strobing is disabled.
class ColorField final : public ProcScene {
public:
    const char* name() const override { return "ColorField"; }
    float costEstimate() const override { return 1.0f; }
    bool usesStrobe() const override { return true; }
protected:
    void initState(ss::Rng& rng) override {
        for (int i = 0; i < 4; ++i) { period_[i] = 9000u + static_cast<std::uint32_t>(i) * 2300u + rng.below(2000); off_[i] = static_cast<std::uint32_t>(rng.next() & 0xFFFF); }
    }
    void draw(const Canvas& cv, const Drive& d) override {
        Rgb8 corner[4];
        const int spread = 1500 + d.intensQ8 * 25;
        const int val = 190 + (d.low >> 3) + (d.strobeOff ? 0 : (d.onset >> 4));
        for (int i = 0; i < 4; ++i) {
            const int s = sinQ15(d.phase(period_[i]) + off_[i]);
            corner[i] = d.col(static_cast<std::uint32_t>(((s * 6000) >> 15) + i * spread), val);
        }
        int add = (d.flash * 220) >> 8;
        if (d.strobeOff && add > 16) add = 16;      // REQ-S12 fallback: gentle lift, no full-frame flash
        const int step = w_ > 1 ? (255 << 8) / (w_ - 1) : 0;
        for (int y = 0; y < h_; ++y) {
            const int ty = h_ > 1 ? (y * 256) / (h_ - 1) : 0;
            const Rgb8 L = lerp(corner[0], corner[2], ty), R = lerp(corner[1], corner[3], ty);
            std::uint8_t* o = cv.at(0, y);
            for (int x = 0; x < w_; ++x, o += 4) {
                const int tx = (x * step) >> 8;
                o[0] = static_cast<std::uint8_t>(clampi(L.r + (((R.r - L.r) * tx) >> 8) + add, 0, 255));
                o[1] = static_cast<std::uint8_t>(clampi(L.g + (((R.g - L.g) * tx) >> 8) + add, 0, 255));
                o[2] = static_cast<std::uint8_t>(clampi(L.b + (((R.b - L.b) * tx) >> 8) + add, 0, 255));
            }
        }
    }
private:
    std::uint32_t period_[4] = {}, off_[4] = {};
};

// Rotating rays from the centre. Onset events blast the rays and flash the frame unless strobing is disabled.
class Starburst final : public ProcScene {
public:
    const char* name() const override { return "Starburst"; }
    float costEstimate() const override { return 2.7f; }
    bool usesStrobe() const override { return true; }
protected:
    void allocate() override { lut_.build(w_, h_); }
    void initState(ss::Rng& rng) override { rays_ = 12 + 4 * static_cast<int>(rng.below(4)); }
    void draw(const Canvas& cv, const Drive& d) override {
        Palette256 pal;
        pal.build(d, 0, 26214u, 256);
        const std::uint32_t rot = d.phase(14000);
        int w0 = 2600 + d.intensQ8 * 30 + d.low * 12;
        int len = 200 + (d.mid >> 1);
        int whiteAdd = 0;
        if (d.strobeOff) { w0 += d.flash * 10; len += d.flash >> 3; }
        else { w0 += d.flash * 60; len += d.flash >> 1; whiteAdd = (d.flash * 140) >> 8; }
        w0 = clampi(w0, 1, 30000);
        const int invW = (1 << 24) / w0, invLen = 65536 / len;
        for (int y = 0; y < h_; ++y) {
            std::uint8_t* o = cv.at(0, y);
            const std::size_t base = std::size_t(y) * w_;
            for (int x = 0; x < w_; ++x, o += 4) {
                const int rn = lut_.radN[base + x];
                const std::uint32_t t = ((lut_.ang[base + x] + rot) & 0xFFFFu) * std::uint32_t(rays_);
                const int idx = static_cast<int>(t >> 16), f = static_cast<int>(t & 0xFFFFu);
                const int c = f < 32768 ? f : 65536 - f;
                int v = 0;
                if (c < w0) {
                    const int ray = ((w0 - c) * invW) >> 16;
                    const int fall = clampi(256 - ((rn * invLen) >> 8), 0, 256);
                    v = (ray * fall) >> 8;
                    if (idx & 1) v = (v * 3) >> 2;
                }
                if (rn < 40) v += ((40 - rn) * (96 + (d.onset >> 1))) >> 5;
                if (v > 256) v = 256;
                const Rgb8 col = pal.c[(idx * 37) & 255];
                o[0] = static_cast<std::uint8_t>(clampi(((col.r * v) >> 8) + whiteAdd, 0, 255));
                o[1] = static_cast<std::uint8_t>(clampi(((col.g * v) >> 8) + whiteAdd, 0, 255));
                o[2] = static_cast<std::uint8_t>(clampi(((col.b * v) >> 8) + whiteAdd, 0, 255));
            }
        }
    }
private:
    PolarLut lut_;
    int rays_ = 16;
};

class PlasmaLite final : public ProcScene {
public:
    const char* name() const override { return "PlasmaLite"; }
    float costEstimate() const override { return 1.1f; }
    bool usesStrobe() const override { return false; }
protected:
    void allocate() override {
        lut_.build(w_, h_);
        cs_.assign(std::size_t(w_), 0);
        rs_.assign(std::size_t(h_), 0);
        for (int i = 0; i < 256; ++i) sin8_[i] = static_cast<std::uint8_t>((sinQ15(static_cast<std::uint32_t>(i) << 8) + 32768) >> 8);
    }
    void initState(ss::Rng& rng) override {
        kx_ = 2 + static_cast<int>(rng.below(3));     // spatial frequencies (cycles across the frame)
        ky_ = 2 + static_cast<int>(rng.below(3));
    }
    void draw(const Canvas& cv, const Drive& d) override {
        Palette256 pal;
        pal.build(d, 0, 16384u + std::uint32_t(d.intensQ8) * 192u, 200 + (d.low >> 3));
        const std::uint32_t t1 = d.phase(7000) >> 8, t2 = d.phase(9500) >> 8, t3 = d.phase(6100) >> 8, t4 = d.phase(11000) >> 8;
        const int kqx = (256 * 256 * kx_) / w_, kqy = (256 * 256 * ky_) / h_, kqd = (256 * 256 * 2) / (w_ + h_);
        for (int x = 0; x < w_; ++x) cs_[std::size_t(x)] = sin8_[((static_cast<std::uint32_t>((x * kqx) >> 8)) + t1) & 255u];
        for (int y = 0; y < h_; ++y) rs_[std::size_t(y)] = sin8_[((static_cast<std::uint32_t>((y * kqy) >> 8)) + t2) & 255u];
        const std::uint32_t shift = static_cast<std::uint32_t>(d.onset >> 2);
        for (int y = 0; y < h_; ++y) {
            std::uint8_t* o = cv.at(0, y);
            const std::size_t base = std::size_t(y) * w_;
            const int ry = rs_[std::size_t(y)];
            for (int x = 0; x < w_; ++x, o += 4) {
                const int dg = sin8_[((static_cast<std::uint32_t>(((x + y) * kqd) >> 8)) + t3) & 255u];
                const int rr = sin8_[((static_cast<std::uint32_t>((lut_.radN[base + x] * 3) >> 1)) + t4) & 255u];
                const int idx = static_cast<int>(((std::uint32_t(cs_[std::size_t(x)] + ry + dg + rr) >> 2) + shift) & 255u);
                const Rgb8 c = pal.c[idx];
                o[0] = c.r; o[1] = c.g; o[2] = c.b;
            }
        }
    }
private:
    PolarLut lut_;
    std::vector<std::uint8_t> cs_, rs_;
    std::uint8_t sin8_[256] = {};
    int kx_ = 3, ky_ = 3;
};

}  // namespace

std::unique_ptr<SceneRenderer> makeTunnel() { return std::make_unique<Tunnel>(); }
std::unique_ptr<SceneRenderer> makeColorField() { return std::make_unique<ColorField>(); }
std::unique_ptr<SceneRenderer> makeStarburst() { return std::make_unique<Starburst>(); }
std::unique_ptr<SceneRenderer> makePlasmaLite() { return std::make_unique<PlasmaLite>(); }

}  // namespace ss::render::detail
