// GeometricOrbit, RadialPulse, ParticleField, NeonGrid.
#include "scenes.hpp"

namespace ss::render::detail {
using namespace fx;

namespace {

class GeometricOrbit final : public ProcScene {
public:
    const char* name() const override { return "GeometricOrbit"; }
    float costEstimate() const override { return 1.2f; }
    bool usesStrobe() const override { return false; }
protected:
    void initState(ss::Rng& rng) override {
        n_ = 5 + static_cast<int>(rng.below(3));
        for (int i = 0; i < kMax; ++i) {
            period_[i] = 4000u + static_cast<std::uint32_t>(i) * 1700u + rng.below(1500);
            off_[i] = static_cast<std::uint32_t>(rng.next() & 0xFFFF);
            ecc_[i] = 170 + static_cast<int>(rng.below(86));
            dir_[i] = (rng.below(2) == 0) ? 1 : -1;
        }
    }
    void draw(const Canvas& cv, const Drive& d) override {
        const int minHalf = (w_ < h_ ? w_ : h_) / 2;
        const int cx = w_ * 128 - 128, cy = h_ * 128 - 128;
        const int pulse = 256 + ((d.low * 80) >> 8) + ((d.onset * 60) >> 8);
        int bx[kMax], by[kMax], rr[kMax];
        for (int i = 0; i < n_; ++i) {
            const int r = ((minHalf * (35 + (55 * (i + 1)) / n_) / 100) * pulse) >> 8;
            rr[i] = r;
            const std::uint32_t a = d.phase(period_[i]) * static_cast<std::uint32_t>(dir_[i]) + off_[i];
            bx[i] = cx + ((r * cosQ15(a)) >> 7);
            by[i] = cy + ((((r * ecc_[i]) >> 8) * sinQ15(a)) >> 7);
        }
        for (int i = 0; i < n_; ++i) ringAA(cv, cx, cy, rr[i] * 256, 128, d.col(static_cast<std::uint32_t>(i) * 3000u, 28), Blend::Add);
        for (int i = 0; i < n_; ++i) {
            const int j = (i + 1) % n_;
            lineAA(cv, bx[i], by[i], bx[j], by[j], d.col(static_cast<std::uint32_t>(i) * 4200u, 90 + (d.intensQ8 >> 1) + (d.mid >> 2)), Blend::Add, 1);
        }
        const int bodyR = 3 * 256 + (d.high * 2) + d.intensQ8 * 3;
        for (int i = 0; i < n_; ++i) {
            const Rgb8 c = d.col(static_cast<std::uint32_t>(i) * 4200u, 250);
            glowDisc(cv, bx[i], by[i], bodyR * 4, c, 110 + (d.onset >> 1));
            fillCircleAA(cv, bx[i], by[i], bodyR, c, Blend::Over);
        }
        ringAA(cv, cx, cy, (minHalf * (12 + (d.low >> 4)) / 100) * 256, 2 * 256, d.col(0x8000u, 240), Blend::Add);
    }
private:
    static constexpr int kMax = 8;
    int n_ = 6;
    std::uint32_t period_[kMax] = {}, off_[kMax] = {};
    int ecc_[kMax] = {}, dir_[kMax] = {};
};

class RadialPulse final : public ProcScene {
public:
    const char* name() const override { return "RadialPulse"; }
    float costEstimate() const override { return 1.4f; }
    bool usesStrobe() const override { return false; }
protected:
    void allocate() override { lut_.build(w_, h_); }
    void draw(const Canvas& cv, const Drive& d) override {
        Palette256 pal;
        pal.build(d, 0, 32768u, 256);
        constexpr int S = 64;                          // ring spacing in Q8 radius units
        const int off = static_cast<int>((d.phase(2600) * std::uint32_t(S)) >> 16);
        const int gain = 100 + (d.intensQ8 >> 1) + ((d.low * 106) >> 8);
        const int ws = w_;
        for (int y = 0; y < h_; ++y) {
            std::uint8_t* o = cv.at(0, y);
            const std::size_t base = std::size_t(y) * ws;
            for (int x = 0; x < ws; ++x, o += 4) {
                const int rn = lut_.radN[base + x];
                const int u = rn + S - off;
                const int f = u & (S - 1), k = u >> 6;
                const int tri = f < S / 2 ? f : S - f;
                int v = (tri * tri) >> 2;                          // 0..256
                const int fade = clampi(300 - rn, 30, 256);
                v = (((v * fade) >> 8) * gain) >> 8;
                if (rn < 80) v += ((80 - rn) * (d.onset + (d.low >> 1))) >> 6;
                if (v > 256) v = 256;
                const Rgb8 c = pal.c[(k * 23) & 255];
                o[0] = static_cast<std::uint8_t>((c.r * v) >> 8);
                o[1] = static_cast<std::uint8_t>((c.g * v) >> 8);
                o[2] = static_cast<std::uint8_t>((c.b * v) >> 8);
            }
        }
    }
private:
    PolarLut lut_;
};

class ParticleField final : public ProcScene {
public:
    const char* name() const override { return "ParticleField"; }
    float costEstimate() const override { return 0.8f; }
    bool usesStrobe() const override { return false; }
protected:
    void initState(ss::Rng& rng) override {
        for (auto& p : p_) {
            p.x0 = static_cast<std::uint16_t>(rng.next() & 0xFFFF);
            p.y0 = static_cast<std::uint16_t>(rng.next() & 0xFFFF);
            const int sx = rng.below(2) ? 1 : -1, sy = rng.below(2) ? 1 : -1;
            p.vx = static_cast<std::int16_t>(sx * (1500 + static_cast<int>(rng.below(4500))));
            p.vy = static_cast<std::int16_t>(sy * (1500 + static_cast<int>(rng.below(4500))));
            p.size = static_cast<std::uint8_t>(2 + rng.below(4));
            p.hue = static_cast<std::uint16_t>(rng.below(12000));
            p.twPeriod = static_cast<std::uint16_t>(800 + rng.below(1700));
            p.twOff = static_cast<std::uint16_t>(rng.next() & 0xFFFF);
        }
    }
    void draw(const Canvas& cv, const Drive& d) override {
        const int count = 48 + ((112 * d.intensQ8) >> 8);
        const std::int64_t tMs = d.timeNs / kNsPerMs;
        const std::int64_t sc = tMs * d.speedQ8;      // ms * Q8
        for (int i = 0; i < count && i < kMax; ++i) {
            const P& p = p_[i];
            const std::int64_t mx = (std::int64_t(p.vx) * sc) / 256000, my = (std::int64_t(p.vy) * sc) / 256000;
            const std::uint32_t px = static_cast<std::uint32_t>(p.x0 + mx) & 0xFFFFu, py = static_cast<std::uint32_t>(p.y0 + my) & 0xFFFFu;
            const int xq = static_cast<int>((std::uint64_t(px) * std::uint32_t(w_ > 1 ? w_ - 1 : 1) * 256u) >> 16);
            const int yq = static_cast<int>((std::uint64_t(py) * std::uint32_t(h_ > 1 ? h_ - 1 : 1) * 256u) >> 16);
            const std::uint32_t tw = d.phase(p.twPeriod) + p.twOff;
            const int twink = 160 + ((sinQ15(tw) * 96) >> 15);
            const int inten = clampi(twink + (d.onset >> 1), 0, 256);
            const int rq = p.size * 160 + 256 + d.high * 2;
            const Rgb8 c = d.col(p.hue, 250);
            glowDisc(cv, xq, yq, rq, c, inten);
            fillCircleAA(cv, xq, yq, 150 + (p.size * 20), scale(c, inten), Blend::Add);
        }
    }
private:
    static constexpr int kMax = 160;
    struct P { std::uint16_t x0, y0; std::int16_t vx, vy; std::uint8_t size; std::uint16_t hue, twPeriod, twOff; };
    P p_[kMax] = {};
};

class NeonGrid final : public ProcScene {
public:
    const char* name() const override { return "NeonGrid"; }
    float costEstimate() const override { return 2.0f; }
    bool usesStrobe() const override { return false; }
protected:
    void draw(const Canvas& cv, const Drive& d) override {
        const int yh = (h_ * 42) / 100, cxq = w_ * 128 - 128;
        gradientV(cv, 0, 0, w_, yh, d.col(0x0800u, 24), d.col(0x0800u, 70 + (d.low >> 2)));
        gradientV(cv, 0, yh, w_, h_ - yh, d.col(0x0800u, 36), d.col(0x0800u, 8));
        glowDisc(cv, cxq, yh * 256, (h_ * 30 / 100) * 256, d.col(0x1800u, 256), 110 + (d.mid >> 1));
        // converging verticals
        for (int i = -7; i <= 7; ++i) {
            const int xt = cxq + (i * w_ * 256) / 40, xb = cxq + (i * w_ * 256) / 7;
            lineAA(cv, xt, yh * 256, xb, (h_ - 1) * 256, d.col(0, 60 + (d.intensQ8 >> 1) + (d.low >> 2)), Blend::Add, 1);
        }
        // horizontals scroll toward the viewer with elapsed time
        constexpr int n = 10;
        const std::uint32_t ph = d.phase(4000);
        for (int k = 0; k < n; ++k) {
            const std::uint32_t u = (static_cast<std::uint32_t>(k) * 65536u / n + ph) & 0xFFFFu;
            const std::uint32_t t = (u * u) >> 16;             // perspective: spacing grows toward the viewer
            const int y = yh * 256 + static_cast<int>((std::uint64_t(h_ - 1 - yh) * 256u * t) >> 16);
            const int v = 40 + (d.intensQ8 >> 2) + static_cast<int>((t * 150u) >> 16) + (d.low >> 2);
            lineAA(cv, 0, y, (w_ - 1) * 256, y, d.col(0, v), Blend::Add, t > 40000u ? 2 : 1);
        }
        lineAA(cv, 0, yh * 256, (w_ - 1) * 256, yh * 256, d.col(0x1800u, 220), Blend::Add, 1);
    }
};

}  // namespace

std::unique_ptr<SceneRenderer> makeGeometricOrbit() { return std::make_unique<GeometricOrbit>(); }
std::unique_ptr<SceneRenderer> makeRadialPulse() { return std::make_unique<RadialPulse>(); }
std::unique_ptr<SceneRenderer> makeParticleField() { return std::make_unique<ParticleField>(); }
std::unique_ptr<SceneRenderer> makeNeonGrid() { return std::make_unique<NeonGrid>(); }

}  // namespace ss::render::detail
