// Key estimator (MOD-03): decimated (/8 from its input rate) mono -> 2048-pt peak-picked chroma ->
// Temperley major/minor template correlation over an exponentially weighted 8-20 s context.
// Reports Unknown (valid()==false) below the confidence threshold; no key from noise/drums/silence.
#pragma once
#include <cstdint>
#include <vector>
#include "core/analysis/dsp.h"

namespace ss {
namespace analysis {

class KeyEstimator {
 public:
  explicit KeyEstimator(double inRate);   // sample rate of the mono samples passed to push()
  void reset();
  void flushStream();                     // discontinuity: drop partial frame/decimator state, keep chroma context
  void push(float x);                     // O(1) per sample; a heavy frame runs every ~1.4 s
  bool valid() const { return valid_; }
  int pitchClass() const { return pc_; } // 0=C .. 11=B
  bool minor() const { return minor_; }
  float confidence() const { return conf_; }
  uint32_t updates() const { return updates_; }   // increments whenever the estimate was refreshed
  float lastR() const { return lastR_; }          // best template correlation (diagnostic)
  float lastMargin() const { return lastMargin_; }
 private:
  void frame();
  void evaluate();
  double fk_;
  dsp::HalfBand hb_[3]; int stage_ = 0;
  dsp::Fft fft_;
  std::vector<float> win_, ring_, pwr_, mag_, cum_;
  int rpos_ = 0; int sinceFrame_ = 0; uint64_t total_ = 0;
  int kLo_, kHi_; float cbin_[12];
  std::vector<int> binLo_; std::vector<float> binW_;   // per bin: lower pitch class, weight on lower
  double chroma_[12]; double wAcc_ = 0, decay_, frameSec_, spanSec_ = 0, silentSec_ = 0;
  bool valid_ = false; int pc_ = 0; bool minor_ = false; float conf_ = 0, lastR_ = 0, lastMargin_ = 0;
  int pendKey_ = -1, pendCount_ = 0; uint32_t updates_ = 0;
};

} // namespace analysis
} // namespace ss
