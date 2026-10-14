#include "core/analysis/dsp.h"

namespace ss {
namespace dsp {

Fft::Fft(int n) : n_(n), rev_(n), re_(n, 0.f), im_(n, 0.f) {
  int bits = 0; while ((1 << bits) < n) ++bits;
  for (int i = 0; i < n; ++i) {
    int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[i] = r;
  }
  const double tp = 6.283185307179586476925286766559;
  for (int len = 16; len <= n; len <<= 1) {
    const int half = len / 2;
    for (int j = 0; j < half; ++j) tw_.push_back((float)std::cos(tp * j / len));
    for (int j = 0; j < half; ++j) tw_.push_back((float)-std::sin(tp * j / len));
  }
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
  const float c = 0.70710678118654752f;
  for (int i = 0; i < n; i += 8) {                       // stage 3: w = 1, c(1-j), -j, -c(1+j)
    float* r = re + i; float* m = im + i;
    float tr, ti;
    tr = r[4]; ti = m[4];                                 r[4] = r[0] - tr; m[4] = m[0] - ti; r[0] += tr; m[0] += ti;
    tr = c * (r[5] + m[5]); ti = c * (m[5] - r[5]);       r[5] = r[1] - tr; m[5] = m[1] - ti; r[1] += tr; m[1] += ti;
    tr = m[6]; ti = -r[6];                                r[6] = r[2] - tr; m[6] = m[2] - ti; r[2] += tr; m[2] += ti;
    tr = c * (m[7] - r[7]); ti = -c * (r[7] + m[7]);      r[7] = r[3] - tr; m[7] = m[3] - ti; r[3] += tr; m[3] += ti;
  }
  const float* tw = tw_.data();                          // per-stage contiguous twiddles (len = 16, 32, ...)
  for (int len = 16; len <= n; len <<= 1) {
    const int half = len >> 1;
    for (int i = 0; i < n; i += len) {
      float* ar = re + i; float* ai = im + i;
      float* br = ar + half; float* bi = ai + half;
      const float* wr = tw; const float* wi = tw + half;
      for (int j = 0; j < half; ++j) {
        const float tr = br[j] * wr[j] - bi[j] * wi[j];
        const float ti = br[j] * wi[j] + bi[j] * wr[j];
        br[j] = ar[j] - tr; bi[j] = ai[j] - ti;
        ar[j] += tr; ai[j] += ti;
      }
    }
    tw += len;
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
