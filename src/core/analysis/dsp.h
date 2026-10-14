// Small scalar DSP kernels for MOD-03 (float, no SIMD, i586/x87 friendly). Internal to core/analysis.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

namespace ss {
namespace dsp {

// True when x is neither NaN nor Inf (exponent test; safe without <cmath> FP flags).
inline bool isFinite(float x) { uint32_t u; std::memcpy(&u, &x, 4); return (u & 0x7F800000u) != 0x7F800000u; }
// NaN/Inf -> 0 (counted by caller); tiny values flushed to avoid denormal stalls.
inline float sanitize(float x, uint64_t& count) {
  if (!isFinite(x)) { ++count; return 0.0f; }
  if (x > -1e-20f && x < 1e-20f) return 0.0f;
  return x;
}
// ~1e-4 absolute error log2 for positive normal x.
inline float fastLog2(float x) {
  uint32_t u; std::memcpy(&u, &x, 4);
  const float e = (float)((int)(u >> 23) - 127);
  u = (u & 0x007FFFFFu) | 0x3F800000u;
  float m; std::memcpy(&m, &u, 4);              // m in [1,2)
  const float t = m - 1.0f;
  const float p = 2.03179730e-04f + t * (1.43610783f + t * (-0.669542276f + t * (0.312240972f + t * -0.0791581614f)));
  return p + e;
}

// Radix-2 DIT complex FFT, size power of two (>=8), precomputed twiddles + bit reversal. Caller fills
// re()/im() at positions rev()[j] (bit-reversed load), then run(). Forward transform, unscaled.
class Fft {
 public:
  explicit Fft(int n);
  int size() const { return n_; }
  float* re() { return re_.data(); }
  float* im() { return im_.data(); }
  const int* rev() const { return rev_.data(); }
  void run();
 private:
  int n_;
  std::vector<int> rev_;
  std::vector<float> tw_, re_, im_;
};

// 15-tap half-band FIR decimate-by-2 (-6 dB at fs_in/4). push() returns true every 2nd sample.
class HalfBand {
 public:
  HalfBand();
  void reset();
  bool push(float x, float& y) {
    pos_ = (pos_ + 1) & 15;
    buf_[pos_] = x; buf_[pos_ + 16] = x;
    phase_ ^= 1;
    if (phase_) return false;
    const float* w = buf_ + pos_ + 2;            // w[14] newest (ring of 16, window of 15)
    y = c_ * w[7] + h1_ * (w[6] + w[8]) + h3_ * (w[4] + w[10]) + h5_ * (w[2] + w[12]) + h7_ * (w[0] + w[14]);
    return true;
  }
 private:
  float buf_[32];
  int pos_ = 0, phase_ = 0;
  float c_, h1_, h3_, h5_, h7_;
};

} // namespace dsp
} // namespace ss
