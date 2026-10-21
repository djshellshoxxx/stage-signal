#include "ss/audio/fft.hpp"
#include <cmath>

namespace ss::audio {

bool Fft::prepare(std::size_t n) {
    n_ = 0;
    if (n < 8 || n > 65536 || (n & (n - 1)) != 0) return false;
    cos_.assign(n / 2, 0.f);
    sin_.assign(n / 2, 0.f);
    const double pi = 3.14159265358979323846;
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double a = -2.0 * pi * static_cast<double>(k) / static_cast<double>(n);
        cos_[k] = static_cast<float>(std::cos(a));
        sin_[k] = static_cast<float>(std::sin(a));
    }
    rev_.assign(n, 0);
    std::size_t bits = 0;
    while ((std::size_t(1) << bits) < n) ++bits;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b) if (i & (std::size_t(1) << b)) r |= std::size_t(1) << (bits - 1 - b);
        rev_[i] = r;
    }
    re_.assign(n, 0.f);
    im_.assign(n, 0.f);
    n_ = n;
    return true;
}

void Fft::forward(float* re, float* im) const {
    const std::size_t n = n_;
    if (n == 0) return;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = rev_[i];
        if (j > i) {
            const float tr = re[i]; re[i] = re[j]; re[j] = tr;
            const float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len >> 1, step = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const float wr = cos_[j * step], wi = sin_[j * step];
                const std::size_t p = i + j, q = p + half;
                const float tr = re[q] * wr - im[q] * wi;
                const float ti = re[q] * wi + im[q] * wr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr;        im[p] += ti;
            }
        }
    }
}

void Fft::powerSpectra(const float* a, const float* b, float* pa, float* pb) {
    const std::size_t n = n_;
    if (n == 0) return;
    for (std::size_t i = 0; i < n; ++i) { re_[i] = a[i]; im_[i] = b ? b[i] : 0.f; }
    forward(re_.data(), im_.data());
    const std::size_t h = n / 2;
    for (std::size_t k = 0; k <= h; ++k) {
        const std::size_t m = (n - k) & (n - 1);           // mirror index (0 -> 0)
        const float zr = re_[k], zi = im_[k], cr = re_[m], ci = -im_[m];   // Z[k], conj(Z[N-k])
        const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);          // spectrum of a
        pa[k] = er * er + ei * ei;
        if (b && pb) {
            const float or_ = 0.5f * (zi - ci), oi = -0.5f * (zr - cr);    // (Z - conj)/(2i) = spectrum of b
            pb[k] = or_ * or_ + oi * oi;
        }
    }
}

}  // namespace ss::audio
