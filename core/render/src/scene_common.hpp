// Internal helpers shared by the procedural scenes. Not installed / not part of the public contract.
#pragma once
#include <vector>
#include "ss/base/rng.hpp"
#include "ss/render/fixed.hpp"
#include "ss/render/raster.hpp"
#include "ss/render/scene.hpp"

namespace ss::render::detail {

// Decaying flash envelope fed by FeatureSnapshot::onsetFired. Decay is a function of elapsed time only.
struct OnsetEnv {
    int peakQ8 = 0;
    Ns atNs = 0;
    bool has = false;
};

// Everything a scene needs, digested once per frame from SceneInput into integers (Q8 unless noted).
struct Drive {
    int w = 0, h = 0;
    Ns timeNs = 0;
    int speedQ8 = 256;          // 0..2048
    int intensQ8 = 0;           // 0..256
    int brightQ8 = 256;         // 0..256 (0 => black frame)
    int satQ8 = 0;              // 0..256
    std::uint32_t hueQ16 = 0;   // turns
    int spec[32] = {};          // 0..256
    int wave[256] = {};         // -256..256
    int low = 0, mid = 0, high = 0;   // 0..256
    int onset = 0;              // 0..256 smooth-ish onset level (strength or soft decay)
    int flash = 0;              // 0..256 hard flash pulse, ONLY from real onsetFired events (never idle)
    bool hasAudio = false;
    bool strobeOff = false;

    // Colour from the scene hue with an offset; value is scaled by master brightness.
    fx::Rgb8 col(std::uint32_t hueOff, int valQ8) const {
        return fx::hsv(hueQ16 + hueOff, satQ8, (fx::clampi(valQ8, 0, 256) * brightQ8) >> 8);
    }
    fx::Rgb8 colAbs(std::uint32_t hue, int valQ8) const {
        return fx::hsv(hue, satQ8, (fx::clampi(valQ8, 0, 256) * brightQ8) >> 8);
    }
    std::uint32_t phase(std::uint32_t periodMs) const { return fx::phaseQ16(timeNs, speedQ8, periodMs); }
};

void digest(const SceneInput& in, OnsetEnv& env, Drive& d);

// 256-entry palette spanning `spanQ16` turns of hue from the scene hue; value 0..256 scales with brightness.
struct Palette256 {
    fx::Rgb8 c[256];
    void build(const Drive& d, std::uint32_t hueOff, std::uint32_t spanQ16, int valQ8) {
        for (int i = 0; i < 256; ++i)
            c[i] = d.col(hueOff + static_cast<std::uint32_t>((std::uint64_t(spanQ16) * i) >> 8), valQ8);
    }
};

// Per-pixel polar coordinates about the frame centre.
struct PolarLut {
    std::vector<std::uint16_t> ang;   // turns Q16
    std::vector<std::uint16_t> rad;   // distance from centre in 1/16 pixel
    std::vector<std::uint16_t> radN;  // radius normalised to Q8 where 256 = min(w,h)/2 (corners ~ 360)
    int halfMin16 = 16;               // min(w,h)/2 in 1/16 pixel (>=16)
    void build(int w, int h);
};

class ProcScene : public SceneRenderer {
public:
    void prepare(std::uint32_t w, std::uint32_t h, std::uint64_t seed) final;
    void render(const SceneInput& in, ss::PixelBuffer& out) final;
protected:
    virtual void allocate() {}                       // all heap use lives here
    virtual void initState(ss::Rng&) {}              // seeded state; must not allocate
    virtual void draw(const Canvas& cv, const Drive& d) = 0;
    int w_ = 0, h_ = 0;
    std::uint64_t seed_ = 1;
    OnsetEnv env_;
};

}  // namespace ss::render::detail
