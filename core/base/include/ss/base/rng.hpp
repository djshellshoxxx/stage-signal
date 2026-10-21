// StageSignal base: small deterministic PRNG so seeded scenes/pools are reproducible (REQ-S01).
#pragma once
#include <cstdint>

namespace ss {

// splitmix64 seeding + xoshiro256** generator. Identical output on every platform/compiler.
class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) { reseed(seed); }
    void reseed(std::uint64_t seed) {
        for (auto& s : s_) { seed += 0x9E3779B97F4A7C15ULL; std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; s = z ^ (z >> 31); }
    }
    std::uint64_t next() {
        const std::uint64_t r = rotl(s_[1] * 5, 7) * 9, t = s_[1] << 17;
        s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3]; s_[2] ^= t; s_[3] = rotl(s_[3], 45);
        return r;
    }
    // Uniform integer in [0, n). n must be > 0.
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>((next() >> 32) * n >> 32); }
    // Uniform float in [0, 1).
    float unit() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4]{};
};

}  // namespace ss
