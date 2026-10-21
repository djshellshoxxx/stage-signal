// SpectrumBars, MirrorBars, WaveformRibbon, LissajousScope: audio-shape driven scenes.
#include "scenes.hpp"

namespace ss::render::detail {
using namespace fx;

namespace {

int barGap(int w) { const int g = w / 128; return g < 1 ? 1 : g; }

class SpectrumBars final : public ProcScene {
public:
    const char* name() const override { return "SpectrumBars"; }
    float costEstimate() const override { return 0.45f; }
    bool usesStrobe() const override { return false; }
protected:
    void draw(const Canvas& cv, const Drive& d) override {
        constexpr int n = 32;
        const int gap = barGap(w_);
        int bw = (w_ - gap * (n + 1)) / n;
        if (bw < 1) bw = 1;
        const int total = n * bw + (n + 1) * gap;
        const int x0 = (w_ - total) / 2 + gap;
        const int margin = h_ / 16, hmax = h_ - 2 * margin;
        const int gain = 192 + d.intensQ8 / 2;
        const int capH = h_ / 60 < 1 ? 1 : h_ / 60;
        for (int i = 0; i < n; ++i) {
            const int lvl = clampi((d.spec[i] * gain) >> 8, 0, 256);
            int hgt = (hmax * lvl) >> 8;
            if (hgt < 2) hgt = 2;
            const int x = x0 + i * (bw + gap), yTop = h_ - margin - hgt;
            const std::uint32_t off = static_cast<std::uint32_t>(i) * 700u;
            const int val = 200 + (d.onset >> 2);
            gradientV(cv, x, yTop, bw, hgt, d.col(off, val), d.col(off + 3000u, 90));
            fillRect(cv, x, yTop, bw, capH, hsv(d.hueQ16 + off, d.satQ8 / 2, d.brightQ8));
        }
    }
};

class MirrorBars final : public ProcScene {
public:
    const char* name() const override { return "MirrorBars"; }
    float costEstimate() const override { return 0.25f; }
    bool usesStrobe() const override { return false; }
protected:
    void draw(const Canvas& cv, const Drive& d) override {
        constexpr int per = 16;
        const int gap = barGap(w_);
        int bw = (w_ / 2 - gap * (per + 1)) / per;
        if (bw < 1) bw = 1;
        const int cx = w_ / 2, cy = h_ / 2, margin = h_ / 16;
        const int halfMax = cy - margin;
        const int gain = 192 + d.intensQ8 / 2;
        for (int k = 0; k < per; ++k) {
            const int s = (d.spec[2 * k] + d.spec[2 * k + 1]) >> 1;
            const int lvl = clampi((s * gain) >> 8, 0, 256);
            int hgt = (halfMax * lvl) >> 8;
            if (hgt < 1) hgt = 1;
            const std::uint32_t off = static_cast<std::uint32_t>(k) * 1400u;
            const Rgb8 centre = d.col(off, 230 + (d.onset >> 3)), tip = d.col(off + 4000u, 80);
            const int xr = cx + gap + k * (bw + gap), xl = cx - gap - bw - k * (bw + gap);
            for (int side = 0; side < 2; ++side) {
                const int x = side ? xr : xl;
                gradientV(cv, x, cy - hgt, bw, hgt, tip, centre);
                gradientV(cv, x, cy, bw, hgt, centre, tip);
            }
        }
        fillRect(cv, 0, cy, w_, 1, d.col(0, 70), Blend::Add);
    }
};

class WaveformRibbon final : public ProcScene {
public:
    const char* name() const override { return "WaveformRibbon"; }
    float costEstimate() const override { return 0.45f; }
    bool usesStrobe() const override { return false; }
protected:
    void draw(const Canvas& cv, const Drive& d) override {
        constexpr int n = 48, layers = 3;
        const std::uint32_t ph = d.phase(5200), ph2 = d.phase(7300);
        const int amp = ((h_ * (96 + d.intensQ8 / 2)) / 3) >> 8;      // peak deflection in pixels
        const int drift = h_ / 12, span = (h_ * 10) / 100;
        for (int l = 0; l < layers; ++l) {
            int px[n], py[n];
            const int lo = ((l - 1) * span * sinQ15(ph2 + static_cast<std::uint32_t>(l) * 21845u)) >> 15;
            for (int i = 0; i < n; ++i) {
                const int idx = (i * 252) / (n - 1);
                const int s = (d.wave[idx] + d.wave[idx + 1] + d.wave[idx + 2] + d.wave[idx + 3]) >> 2;   // -256..256
                const int dr = (drift * sinQ15(ph + static_cast<std::uint32_t>(i) * 1400u + static_cast<std::uint32_t>(l) * 9000u)) >> 15;
                px[i] = ((w_ - 1) * 256 * i) / (n - 1);
                py[i] = (h_ / 2 + lo + dr + ((amp * s) >> 8)) * 256;
            }
            const Rgb8 c = d.col(static_cast<std::uint32_t>(l) * 5000u, 230 - l * 40 + (d.onset >> 2));
            for (int i = 0; i + 1 < n; ++i) lineAA(cv, px[i], py[i], px[i + 1], py[i + 1], c, Blend::Add, 2);
        }
    }
};

class LissajousScope final : public ProcScene {
public:
    const char* name() const override { return "LissajousScope"; }
    float costEstimate() const override { return 1.2f; }
    bool usesStrobe() const override { return false; }
protected:
    void initState(ss::Rng& rng) override {
        a_ = 1 + static_cast<int>(rng.below(5));
        b_ = 2 + static_cast<int>(rng.below(5));
        if (b_ == a_) b_ = a_ + 1;
    }
    void draw(const Canvas& cv, const Drive& d) override {
        constexpr int n = 320;
        const std::uint32_t phi = d.phase(9000);
        const int half = (w_ < h_ ? w_ : h_) / 2;
        const int ampBase = (half * 80) / 100;
        const int amp = ampBase * (200 + (d.low >> 2)) / 256;
        const int cx = w_ * 128 - 128, cy = h_ * 128 - 128;   // centre in Q8 pixel-centre space
        int lx = 0, ly = 0;
        for (int i = 0; i <= n; ++i) {
            const std::uint32_t u = static_cast<std::uint32_t>((std::uint32_t(i % n) * 65536u) / n);
            const int wv = d.wave[(i * 255 / n) & 255];
            const int radial = 256 + ((wv * d.intensQ8) >> 10);          // waveform wobbles the radius
            const int sx = sinQ15(static_cast<std::uint32_t>(a_) * u + phi);
            const int sy = sinQ15(static_cast<std::uint32_t>(b_) * u);
            const int x = cx + static_cast<int>(((std::int64_t(amp) * 256 * sx >> 15) * radial) >> 8);
            const int y = cy + static_cast<int>(((std::int64_t(amp) * 256 * sy >> 15) * radial) >> 8);
            if (i > 0) {
                const Rgb8 c = d.col(static_cast<std::uint32_t>(i) * 180u, 210 + (d.onset >> 3));
                lineAA(cv, lx, ly, x, y, scale(c, 90), Blend::Add, 3);   // soft halo
                lineAA(cv, lx, ly, x, y, c, Blend::Add, 1);
            }
            lx = x; ly = y;
        }
    }
private:
    int a_ = 3, b_ = 2;
};

}  // namespace

std::unique_ptr<SceneRenderer> makeSpectrumBars() { return std::make_unique<SpectrumBars>(); }
std::unique_ptr<SceneRenderer> makeMirrorBars() { return std::make_unique<MirrorBars>(); }
std::unique_ptr<SceneRenderer> makeWaveformRibbon() { return std::make_unique<WaveformRibbon>(); }
std::unique_ptr<SceneRenderer> makeLissajousScope() { return std::make_unique<LissajousScope>(); }

}  // namespace ss::render::detail
