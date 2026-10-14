#include "core/analysis/dsp.h"

namespace ss {
namespace dsp {

Fft::Fft(int n) : n_(n), rev_(n), wr_(n / 2), wi_(n / 2), re_(n, 0.f), im_(n, 0.f) {
  int bits = 0; while ((1 << bits) < n) ++bits;
  for (int i = 0; i < n; ++i) {
    int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[i] = r;
  }
  const double tp = 6.283185307179586476925286766559;
  for (int k = 0; k < n / 2; ++k) { wr_[k] = (float)std::cos(tp * k / n); wi_[k] = (float)-std::sin(tp * k / n); }
}

void Fft::run() {
  float* re = re_.data(); float* im = im_.data();
  const int n = n_;
  for (int i = 0; i < n; i += 2) {                       // stage 1: w = 1
    const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
    re[i] = ar + br; im[i] = ai + bi; re[i + 1] = ar - br; im[i + 1] = ai - bi;
  }
  for (int i = 0; i < n; i += 4) {                       // stage 2: w = 1, -j
    float ar = re[i], ai = im[i], br = re[i + 2], bi = im[i + 2];
    re[i] = ar + br; im[i] = ai + bi; re[i + 2] = ar - br; im[i + 2] = ai - bi;
    ar = re[i + 1]; ai = im[i + 1]; br = im[i + 3]; bi = -re[i + 3];   // b*(-j) = (bi, -br)
    re[i + 1] = ar + br; im[i + 1] = ai + bi; re[i + 3] = ar - br; im[i + 3] = ai - bi;
  }
  for (int len = 8; len <= n; len <<= 1) {
    const int half = len >> 1, step = n / len;
    for (int i = 0; i < n; i += len) {
      for (int j = 0, t = 0; j < half; ++j, t += step) {
        const float wr = wr_[t], wi = wi_[t];
        float* ar = re + i + j; float* ai = im + i + j;
        float* br = ar + half; float* bi = ai + half;
        const float tr = *br * wr - *bi * wi;
        const float ti = *br * wi + *bi * wr;
        *br = *ar - tr; *bi = *ai - ti;
        *ar += tr; *ai += ti;
      }
    }
  }
}

HalfBand::HalfBand() {
  const double pi = 3.14159265358979323846;
  double h[8] = {0};                                     // h[k], k=1,3,5,7 (odd only)
  double sum = 0.5;
  for (int k = 1; k <= 7; k += 2) {
    const double sinc = std::sin(pi * k / 2.0) / (pi * k / 2.0);
    const double win = 0.54 + 0.46 * std::cos(pi * k / 8.0);   // Hamming over +-8
    h[k] = 0.5 * sinc * win;
    sum += 2.0 * h[k];
  }
  c_ = (float)(0.5 / sum); h1_ = (float)(h[1] / sum); h3_ = (float)(h[3] / sum);
  h5_ = (float)(h[5] / sum); h7_ = (float)(h[7] / sum);
  reset();
}
void HalfBand::reset() { std::memset(buf_, 0, sizeof buf_); pos_ = 0; phase_ = 0; }

} // namespace dsp
} // namespace ss
