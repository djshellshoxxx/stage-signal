#include "scene_common.hpp"
#include <cstring>

namespace ss::render {

SceneRenderer::~SceneRenderer() = default;

namespace detail {
using namespace fx;

void digest(const SceneInput& in, OnsetEnv& env, Drive& d) {
    static const model::SceneParams kDefault{};
    const model::SceneParams& p = in.params ? *in.params : kDefault;
    d.timeNs = in.timeNs < 0 ? 0 : in.timeNs;
    d.speedQ8 = toQ8(p.speed, 8.f);
    d.intensQ8 = toQ8(p.intensity);
    d.brightQ8 = toQ8(p.brightness);
    d.satQ8 = toQ8(p.saturation);
    d.hueQ16 = toTurnsQ16(p.hue);
    d.strobeOff = in.strobingDisabled;
    d.hasAudio = in.audio != nullptr;

    if (in.audio) {
        for (int i = 0; i < 32; ++i) d.spec[i] = toQ8(in.audio->spectrum[i]);
        for (int i = 0; i < 256; ++i) {
            const float v = in.audio->waveform[i];
            d.wave[i] = v >= 1.f ? 256 : (v <= -1.f ? -256 : (v == v ? static_cast<int>(v * 256.f) : 0));
        }
    } else {
        // Idle animation: slow travelling hills, driven by time and speed only (freezes at speed 0).
        const std::uint32_t ph = phaseQ16(d.timeNs, d.speedQ8, 3200);
        for (int i = 0; i < 32; ++i)
            d.spec[i] = 70 + ((sinQ15(ph + static_cast<std::uint32_t>(i) * 2600u) + 32768) * 90 >> 16) +
                        ((sinQ15(ph * 2 + static_cast<std::uint32_t>(i) * 5200u) + 32768) * 30 >> 16);
        const std::uint32_t pw = phaseQ16(d.timeNs, d.speedQ8, 1800);
        for (int i = 0; i < 256; ++i)
            d.wave[i] = (sinQ15(pw + static_cast<std::uint32_t>(i) * 768u) >> 8) * 3 / 4 +
                        (sinQ15(pw * 3 + static_cast<std::uint32_t>(i) * 2304u) >> 11);
    }

    const FeatureSnapshot* f = in.features;
    if (f) {
        d.low = toQ8(f->band[0]); d.mid = toQ8(f->band[1]); d.high = toQ8(f->band[2]);
    } else {
        int s0 = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < 8; ++i) s0 += d.spec[i];
        for (int i = 8; i < 20; ++i) s1 += d.spec[i];
        for (int i = 20; i < 32; ++i) s2 += d.spec[i];
        d.low = s0 / 8; d.mid = s1 / 12; d.high = s2 / 12;
    }

    // Flash envelope (hard, event driven) and the softer onset level.
    d.flash = 0;
    if (f) {
        if (f->onsetFired) {
            const int s = toQ8(f->onsetStrength);
            env.peakQ8 = s < 160 ? 160 : s;
            env.atNs = d.timeNs;
            env.has = true;
        }
        if (env.has && d.timeNs >= env.atNs) {
            const Ns age = d.timeNs - env.atNs;
            constexpr Ns kDecay = 250 * kNsPerMs;
            if (age < kDecay) d.flash = static_cast<int>((std::int64_t(env.peakQ8) * (kDecay - age)) / kDecay);
        }
        d.onset = d.flash > toQ8(f->onsetStrength) ? d.flash : toQ8(f->onsetStrength);
    } else {
        const Ns ms = (d.timeNs / kNsPerMs) * d.speedQ8 / 256;
        const int phase = static_cast<int>(ms % 600);
        d.onset = (600 - phase) * 90 / 600;   // gentle idle pulse, never a flash
    }
}

void PolarLut::build(int w, int h) {
    const std::size_t n = std::size_t(w) * h;
    ang.assign(n, 0);
    rad.assign(n, 0);
    radN.assign(n, 0);
    const int minHalf = (w < h ? w : h) / 2;
    halfMin16 = minHalf < 1 ? 16 : minHalf * 16;
    std::size_t i = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x, ++i) {
            const std::int32_t dx2 = 2 * x + 1 - w, dy2 = 2 * y + 1 - h;
            ang[i] = static_cast<std::uint16_t>(atan2Q16(dy2, dx2));
            const std::uint64_t r = isqrt64((std::uint64_t(dx2) * dx2 + std::uint64_t(dy2) * dy2) * 64ull);   // 16 * pixels
            rad[i] = static_cast<std::uint16_t>(r > 65535 ? 65535 : r);
            const std::uint64_t rn = (r * 256ull) / static_cast<std::uint64_t>(halfMin16);
            radN[i] = static_cast<std::uint16_t>(rn > 65535 ? 65535 : rn);
        }
}

void ProcScene::prepare(std::uint32_t w, std::uint32_t h, std::uint64_t seed) {
    w_ = static_cast<int>(w > 16384 ? 16384 : w);
    h_ = static_cast<int>(h > 16384 ? 16384 : h);
    seed_ = seed;
    env_ = OnsetEnv{};
    allocate();
    ss::Rng rng(seed_);
    initState(rng);
}

void ProcScene::render(const SceneInput& in, ss::PixelBuffer& out) {
    Canvas cv(out);
    if (!cv.valid()) return;
    if (cv.w != w_ || cv.h != h_) { fill(cv, Rgb8{}); return; }
    if (in.seed != 0 && in.seed != seed_) {
        seed_ = in.seed;
        ss::Rng rng(seed_);
        initState(rng);
    }
    Drive d;
    d.w = w_; d.h = h_;
    digest(in, env_, d);
    if (d.brightQ8 == 0) { fill(cv, Rgb8{}); return; }
    fill(cv, Rgb8{});
    draw(cv, d);
}

}  // namespace detail
}  // namespace ss::render
