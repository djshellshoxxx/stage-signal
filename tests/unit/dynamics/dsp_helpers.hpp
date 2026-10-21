// Shared helpers for the dynamics unit tests.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "ss/base/rng.hpp"
#include "ss/dynamics/processor.hpp"

namespace dyntest {

using namespace ss::dynamics;

constexpr double kPi = 3.14159265358979323846;

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

// A processor wired for offline tests: processes interleaved buffers in fixed-size blocks.
struct Rig {
    DynamicsProcessor proc;
    PrepareInfo info;
    std::uint32_t block = 256;

    explicit Rig(const DynamicsParams& p = {}, DynamicsConfig cfg = {}) {
        proc.setParams(p);
        auto r = proc.prepare(cfg);
        info = r.value();
    }

    std::vector<float> run(const std::vector<float>& in) {
        std::vector<float> out(in.size() / info.inChannels * info.outChannels);
        std::size_t frames = in.size() / info.inChannels;
        for (std::size_t f = 0; f < frames; f += block) {
            const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, frames - f));
            proc.process(in.data() + f * info.inChannels, out.data() + f * info.outChannels, n);
        }
        return out;
    }
};

inline DynamicsParams neutral() {   // compressor transparent (ratio 1), everything else default
    DynamicsParams p;
    p.ratio = 1.0f;
    return p;
}

// stereo interleaved from per-channel generators
template <class FL, class FR>
std::vector<float> stereo(std::size_t frames, FL&& l, FR&& r) {
    std::vector<float> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) { v[2 * i] = static_cast<float>(l(i)); v[2 * i + 1] = static_cast<float>(r(i)); }
    return v;
}

inline std::vector<float> sineStereo(std::size_t frames, double freq, double amp, double fs, double phase = 0.0) {
    return stereo(frames, [&](std::size_t i) { return amp * std::sin(2 * kPi * freq * static_cast<double>(i) / fs + phase); },
                  [&](std::size_t i) { return amp * std::sin(2 * kPi * freq * static_cast<double>(i) / fs + phase); });
}

inline float peakAbs(const std::vector<float>& v, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    float m = 0;
    to = std::min(to, v.size());
    for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(v[i]));
    return m;
}

inline bool allFinite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

}  // namespace dyntest
