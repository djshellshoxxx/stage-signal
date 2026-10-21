// StageSignal audio: scalar radix-2 FFT (own implementation, no third-party code).
#pragma once
#include <cstddef>
#include <vector>

namespace ss::audio {

// In-place iterative radix-2 decimation-in-time complex FFT. prepare() allocates; every other call is
// allocation free. Not thread-safe (owns scratch buffers); one instance per analysis thread.
class Fft {
public:
    // n must be a power of two in [8, 65536]. Returns false (and leaves the object unusable) otherwise.
    bool prepare(std::size_t n);
    std::size_t size() const { return n_; }

    // Forward transform of (re, im), each n floats, in place. X[k] = sum x[n] exp(-2 pi i k n / N).
    void forward(float* re, float* im) const;

    // One-sided power spectra |X[k]|^2, k = 0..n/2, of one or two REAL sequences of length n using a single
    // complex transform (two-for-one packing). b may be nullptr (pb then untouched). pa/pb need n/2+1 floats.
    void powerSpectra(const float* a, const float* b, float* pa, float* pb);

private:
    std::size_t n_ = 0;
    std::vector<float> cos_, sin_;        // exp(-2 pi i k / N), k < N/2
    std::vector<std::size_t> rev_;
    std::vector<float> re_, im_;          // scratch for powerSpectra
};

}  // namespace ss::audio
