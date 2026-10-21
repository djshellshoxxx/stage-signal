// Shared helpers for the render tests.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "doctest/doctest.h"
#include "ss/render/effects.hpp"
#include "ss/render/scene.hpp"

// ---- counting allocator (test_alloc.cpp replaces global operator new/delete) ---------------
namespace alloc_probe {
extern std::size_t count;     // number of operator new calls while armed
extern bool armed;
struct Scope {
    Scope() { count = 0; armed = true; }
    ~Scope() { armed = false; }
    std::size_t n() const { return count; }
};
}  // namespace alloc_probe

namespace rt {

inline std::uint64_t fnv1a(const std::vector<std::uint8_t>& v, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) { h ^= v[i]; h *= 1099511628211ull; }
    return h;
}
inline std::uint64_t hashOf(const ss::PixelBuffer& b) { return fnv1a(b.rgba, std::size_t(b.width) * b.height * 4); }

constexpr ss::model::SceneKind kAllKinds[12] = {
    ss::model::SceneKind::SpectrumBars, ss::model::SceneKind::MirrorBars, ss::model::SceneKind::WaveformRibbon,
    ss::model::SceneKind::GeometricOrbit, ss::model::SceneKind::RadialPulse, ss::model::SceneKind::ParticleField,
    ss::model::SceneKind::NeonGrid, ss::model::SceneKind::Tunnel, ss::model::SceneKind::ColorField,
    ss::model::SceneKind::Starburst, ss::model::SceneKind::PlasmaLite, ss::model::SceneKind::LissajousScope};

// Every value is exactly representable in binary floating point so results do not depend on the compiler.
struct Fixture {
    ss::render::AudioVisualInput audio;
    ss::FeatureSnapshot feat;
    ss::model::SceneParams params;
    Fixture() {
        for (int i = 0; i < 32; ++i) audio.spectrum[i] = static_cast<float>((i * 37 + 11) % 64) / 64.f;
        for (int i = 0; i < 256; ++i) audio.waveform[i] = static_cast<float>((i * 53) % 128 - 64) / 64.f;
        feat.band[0] = 0.5f; feat.band[1] = 0.25f; feat.band[2] = 0.125f;
        feat.onsetStrength = 0.5f; feat.onsetFired = false; feat.silent = false;
        params.brightness = 0.75f; params.speed = 1.5f; params.intensity = 0.625f; params.hue = 0.25f; params.saturation = 0.875f;
    }
    ss::render::SceneInput in(ss::Ns t = 2'500'000'000LL) const {
        ss::render::SceneInput s;
        s.timeNs = t; s.dtSec = 0.04; s.features = &feat; s.audio = &audio; s.params = &params; s.seed = 0xC0FFEEull;
        return s;
    }
};

inline ss::PixelBuffer renderOnce(ss::model::SceneKind k, std::uint32_t w, std::uint32_t h, const ss::render::SceneInput& in,
                                  std::uint64_t seed = 0xC0FFEEull) {
    auto s = ss::render::makeScene(k);
    s->prepare(w, h, seed);
    ss::PixelBuffer out(w, h);
    s->render(in, out);
    return out;
}

inline double meanLuma(const ss::PixelBuffer& b) {
    double sum = 0;
    for (std::size_t i = 0; i < b.rgba.size(); i += 4) sum += (b.rgba[i] + b.rgba[i + 1] + b.rgba[i + 2]) / 3.0;
    return sum / double(b.width * b.height);
}
inline bool allBlack(const ss::PixelBuffer& b) {
    for (std::size_t i = 0; i < b.rgba.size(); i += 4) if (b.rgba[i] | b.rgba[i + 1] | b.rgba[i + 2]) return false;
    return true;
}

// ---- golden hash file ------------------------------------------------------------------------
inline std::string goldenPath() { return std::string(SS_RENDER_GOLDEN_DIR) + "/frames.txt"; }
inline bool updating() { const char* e = std::getenv("SS_UPDATE_GOLDEN"); return e && *e == '1'; }
inline std::map<std::string, std::string>& goldenMap() {
    static std::map<std::string, std::string> m;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(goldenPath());
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string k, v;
            if (ss >> k >> v) m[k] = v;
        }
    }
    return m;
}
inline std::string hex(std::uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b; }
inline void flushGolden() {
    std::ofstream f(goldenPath(), std::ios::trunc);
    f << "# StageSignal render golden frames: FNV-1a 64 of RGBA8 bytes. Regenerate with SS_UPDATE_GOLDEN=1 ./test_render\n";
    for (auto& kv : goldenMap()) f << kv.first << ' ' << kv.second << '\n';
}
// Compares (or, with SS_UPDATE_GOLDEN=1, records) one hash.
inline void checkGolden(const std::string& key, std::uint64_t h) {
    auto& m = goldenMap();
    if (updating()) { m[key] = hex(h); flushGolden(); return; }
    auto it = m.find(key);
    INFO("golden key: " << key << " actual " << hex(h));
    REQUIRE_MESSAGE(it != m.end(), "missing golden entry (run with SS_UPDATE_GOLDEN=1 once and commit)");
    CHECK(it->second == hex(h));
}

}  // namespace rt
