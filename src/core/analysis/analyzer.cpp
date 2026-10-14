#include "core/analysis/analyzer.h"
#include "core/analysis/dsp.h"
#include "core/analysis/tempo.h"
#include "core/analysis/key.h"
#include <algorithm>
#include <cmath>

namespace ss {

namespace {
constexpr int kN = 512;                 // complex FFT size on the /2-decimated stream == 1024 native samples
constexpr int kHop = 128;               // decimated hop == 256 native samples
constexpr float kNearFull = 0.94406088f;     // -0.5 dBFS
constexpr int kClipRun = 3;             // consecutive near-full-scale samples => possible clipping
constexpr float kSilentDb = -70.0f;
constexpr float kEps = 1e-10f;
constexpr float kDcR = 0.999f;
constexpr int kLearnFrames = 40;       // ~0.2 s of flux statistics before onsets are reported
constexpr double kRefractorySec = 0.100;
constexpr int kOnsetLatency = 256 + 192;   // native samples: peak frame is one hop back + ~window-centre offset
inline float db10(double p) { return p > 1e-12 ? (float)(10.0 * std::log10(p)) : kDbSilence; }
inline float db20(double a) { return a > 1e-6 ? (float)(20.0 * std::log10(a)) : kDbSilence; }
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
}

struct Analyzer::Impl {
  uint32_t rate, ch; int ach;
  AnalyzerCounters cnt;
  bool started = false; uint32_t gen = 0;
  bool haveFirst = false; uint64_t nextFirst = 0;
  uint64_t pos = 0;                        // native frames consumed (stream clock, gaps included)
  Ns blockEndNs = 0; bool haveNs = false;

  // ---- decimation + band/flux frames
  dsp::HalfBand hb[2];
  dsp::Fft fft;
  std::vector<float> win, ringL, ringR, prevL;
  int rpos = 0, hopCnt = 0, warm = 0; float dcX[2] = {0, 0}, dcY[2] = {0, 0};
  int kLoB, kMidB, kHiB, kMaxB;            // band edges (bin indices, [lo,hi) )
  float normPow;                           // converts summed channel power to mean-square units
  double bandP[3] = {0, 0, 0};             // smoothed band power (ms units)
  float bandRel;

  // ---- levels (50 ms windows)
  int winLen, winCnt = 0;
  double sumsq[2] = {0, 0}; float peak[2] = {0, 0}; int runLen[2] = {0, 0};
  bool winNear = false, winClip = false; int nearHold = 0, clipHold = 0;
  float rmsDb[2] = {kDbSilence, kDbSilence}, peakDb[2] = {kDbSilence, kDbSilence}, rmsMix = kDbSilence;
  bool silent = true; bool levelsReady = false;
  int silentWins = 0, loudWins = 0;

  // ---- onsets
  float fPrev = 0, fPrev2 = 0, mEma = 0, devEma = 0, thrPrev = 1e9f, mPrev = 0;
  bool haveStats = false; int statFrames = 0;
  int64_t lastOnsetPos = -1000000000LL;
  uint64_t onsetCount = 0, onsetsReported = 0;
  float onsetStr = 0;

  // ---- tempo/key
  analysis::TempoEstimator tempo;
  analysis::KeyEstimator key;
  int tempoFrames = 0, tempoEvery;
  Ns tempoUpdNs = 0, keyUpdNs = 0; uint32_t keyUpdSeen = 0;
  uint64_t seq = 0;
  Ns srcNs = 0;

  Impl(uint32_t r, uint32_t c)
      : rate(r), ch(c), ach(c >= 2 ? 2 : 1), fft(kN),
        tempo((double)r / 256.0), key((double)r / 2.0) {
    win.resize(kN); ringL.assign(kN, 0.f); ringR.assign(kN, 0.f); prevL.assign(kN / 2, 0.f);
    const double tp = 6.283185307179586;
    double sw2 = 0;
    for (int j = 0; j < kN; ++j) { win[j] = (float)(0.5 - 0.5 * std::cos(tp * j / kN)); sw2 += (double)win[j] * win[j]; }
    normPow = (float)(2.0 / (ach * (double)kN * sw2));
    const double df = (double)r / 1024.0;                       // native bin spacing (same as 1024-pt FFT)
    auto bin = [&](double f) { return (int)std::ceil(f / df); };
    kLoB = std::max(1, bin(30.0)); kMidB = bin(250.0); kHiB = bin(2000.0);
    const double fTop = std::min(12000.0, (double)r / 4.0);    // Nyquist-bounded (decimated Nyquist = r/4)
    kMaxB = std::min(kN / 2 - 1, (int)std::floor(fTop / df));
    if (kMidB <= kLoB) kMidB = kLoB + 1;
    if (kHiB <= kMidB) kHiB = kMidB + 1;
    if (kMaxB < kHiB) kMaxB = kHiB;
    winLen = std::max(1, (int)(0.05 * r + 0.5));
    bandRel = 0.85f;
    tempoEvery = std::max(1, (int)((double)r / 256.0));          // ~1 Hz
    resetState();
  }

  void resetState() {
    hb[0].reset(); hb[1].reset();
    std::fill(ringL.begin(), ringL.end(), 0.f); std::fill(ringR.begin(), ringR.end(), 0.f);
    std::fill(prevL.begin(), prevL.end(), 0.f);
    rpos = 0; hopCnt = 0; warm = 0; dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0;
    winCnt = 0; sumsq[0] = sumsq[1] = 0; peak[0] = peak[1] = 0; runLen[0] = runLen[1] = 0;
    winNear = winClip = false;
    fPrev = fPrev2 = 0; haveStats = false; thrPrev = 1e9f;
  }

  void hardReset() {
    resetState();
    for (double& b : bandP) b = 0;
    nearHold = clipHold = 0;
    rmsDb[0] = rmsDb[1] = peakDb[0] = peakDb[1] = rmsMix = kDbSilence; silent = true; levelsReady = false;
    silentWins = loudWins = 0;
    mEma = devEma = 0; statFrames = 0; lastOnsetPos = -1000000000LL; onsetStr = 0;
    tempo.reset(); key.reset(); tempoFrames = 0;
    keyUpdSeen = key.updates(); pos = 0; haveNs = false; haveFirst = false;
  }

  // ------------------------------------------------------------------------------------------------
  void finishWindow() {
    const double inv = 1.0 / winLen;
    double mix = 0;
    for (int c = 0; c < ach; ++c) {
      const double ms = sumsq[c] * inv;
      rmsDb[c] = db10(ms); peakDb[c] = db20(peak[c]); mix += ms;
      sumsq[c] = 0; peak[c] = 0;
    }
    if (ach == 1) { rmsDb[1] = rmsDb[0]; peakDb[1] = peakDb[0]; }
    rmsMix = db10(mix / ach);
    levelsReady = true;
    if (winNear) nearHold = 20; else if (nearHold > 0) --nearHold;       // ~1 s hold
    if (winClip) clipHold = 40; else if (clipHold > 0) --clipHold;       // ~2 s hold
    winNear = winClip = false;
    if (rmsMix < kSilentDb) { ++silentWins; loudWins = 0; } else { ++loudWins; silentWins = 0; }
    silent = rmsMix < kSilentDb;
    if (silentWins * 0.05 >= 2.0) {            // >= 2 s of silence: tempo becomes Unknown (history cleared)
      if (tempo.valid()) tempo.invalidate();
    }
  }

  inline void sampleLevels(float l, float r) {
    const float al = l < 0 ? -l : l;
    sumsq[0] += (double)l * l; if (al > peak[0]) peak[0] = al;
    bool near = al >= kNearFull;
    if (near) { if (++runLen[0] >= kClipRun) winClip = true; } else runLen[0] = 0;
    if (ach > 1) {
      const float ar = r < 0 ? -r : r;
      sumsq[1] += (double)r * r; if (ar > peak[1]) peak[1] = ar;
      if (ar >= kNearFull) { near = true; if (++runLen[1] >= kClipRun) winClip = true; } else runLen[1] = 0;
    }
    if (near) winNear = true;
  }

  void fluxFrame() {
    ++cnt.fftFrames;
    float* re = fft.re(); float* im = fft.im(); const int* rev = fft.rev();
    for (int j = 0; j < kN; ++j) {
      const int idx = (rpos + j) & (kN - 1);
      const float w = win[j];
      re[rev[j]] = ringL[idx] * w;
      im[rev[j]] = ach > 1 ? ringR[idx] * w : 0.f;
    }
    fft.run();
    double bp[3] = {0, 0, 0}; double total = 0;
    float flux = 0; int nb = 0;
    const bool havePrev = warm >= kN / kHop + 1;    // analysis window fully refilled
    for (int k = 1; k <= kMaxB; ++k) {
      const float psum = 0.5f * (re[k] * re[k] + im[k] * im[k] + re[kN - k] * re[kN - k] + im[kN - k] * im[kN - k]);
      const float pn = psum * normPow;
      total += pn;
      if (k < kLoB) {}                                  // below band start (DC bin excluded, <30 Hz)
      else if (k < kMidB) bp[0] += pn;
      else if (k < kHiB) bp[1] += pn;
      else bp[2] += pn;
      const float L = 0.5f * dsp::fastLog2(pn + kEps);
      const float d = L - prevL[k];
      if (d > 0) flux += d;
      prevL[k] = L; ++nb;
    }
    for (int b = 0; b < 3; ++b) bandP[b] = bp[b] >= bandP[b] ? bp[b] : std::max(bp[b], bandP[b] * bandRel);
    const float f = havePrev ? flux / (float)nb : 0.f;
    ++warm;
    onsetDetect(f, total);
  }

  bool havePrevFrame() const { return warm >= kN / kHop + 2; }
  void onsetDetect(float f, double totalPow) {
    // Adaptive threshold: slow-rise/fast-fall mean + deviation of the flux; candidate = local max one frame back.
    const bool quiet = totalPow < 1e-8;                 // < ~-80 dBFS broadband: ignore dither-level flux
    bool fired = false;
    if (haveStats && havePrevFrame() && !quiet && fPrev > fPrev2 && fPrev >= f && fPrev > thrPrev && (fPrev - mPrev) > 0.25f) {
      const int64_t onPos = (int64_t)pos - kOnsetLatency;
      if ((double)(onPos - lastOnsetPos) / rate >= kRefractorySec) {
        fired = true; lastOnsetPos = onPos;
        const float strength = clamp01((fPrev - mPrev) / 3.0f);
        ++onsetCount;
        onsetStr = std::max(onsetStr, strength);
        tempo.pushOnset((double)onPos / rate, std::max(0.05f, strength));
      }
    }
    (void)fired;
    // statistics update (uses current frame f)
    if (!havePrevFrame()) { fPrev2 = fPrev; fPrev = f; return; }
    if (statFrames < kLearnFrames) {                       // fast learning of floor statistics after (re)start
      ++statFrames;
      const float a = 1.0f / (float)statFrames;
      mEma += a * (f - mEma);
      const float dv = std::fabs(f - mEma);
      devEma += a * (dv - devEma);
    } else {
      const float a = f > mEma ? 0.01f : 0.05f;
      mEma += a * (f - mEma);
      const float dv = std::fabs(f - mEma);
      devEma += (dv > devEma ? 0.01f : 0.05f) * (dv - devEma);
    }
    haveStats = statFrames >= kLearnFrames;
    mPrev = mEma;
    thrPrev = mEma + std::max(0.25f, 3.0f * devEma);
    const float nov = f > mEma ? f - mEma : 0.f;
    tempo.pushNovelty(quiet ? 0.f : nov);
    onsetStr *= 0.92f;
    fPrev2 = fPrev; fPrev = f;
  }

  void process(const AudioBlock& b) {
    if (b.rate != rate || b.channels != ch || b.channels == 0) { ++cnt.rejectedBlocks; return; }
    if (started && b.generation < gen) { ++cnt.rejectedBlocks; return; }
    if (!started) { started = true; gen = b.generation; }
    else if (b.generation > gen) { hardReset(); gen = b.generation; }
    ++cnt.blocks;
    const size_t n = b.frames();
    if (b.flags & AF_Discontinuity) {
      ++cnt.discontinuities;
      resetState();                                    // spliced audio must not feed FFT/flux windows
      key.flushStream();                               // drop partial key frame; chroma context is kept
      if (haveFirst && b.firstSample > nextFirst && b.firstSample - nextFirst < (1ull << 32)) pos += b.firstSample - nextFirst;
    }
    haveFirst = true; nextFirst = b.firstSample + n;
    if (n == 0) return;
    const float* s = b.samples.data();
    const size_t C = ch;
    for (size_t i = 0; i < n; ++i) {
      const float l = dsp::sanitize(s[i * C], cnt.sanitized);
      const float r = ach > 1 ? dsp::sanitize(s[i * C + 1], cnt.sanitized) : 0.f;
      sampleLevels(l, r);
      if (++winCnt >= winLen) { winCnt = 0; finishWindow(); }
      float yl, yr = 0;
      const bool ol = hb[0].push(l, yl);
      if (ach > 1) hb[1].push(r, yr);
      if (ol) {
        { const float a = yl - dcX[0] + kDcR * dcY[0]; dcX[0] = yl; dcY[0] = a; yl = a; }       // DC bin leakage control
        if (ach > 1) { const float a = yr - dcX[1] + kDcR * dcY[1]; dcX[1] = yr; dcY[1] = a; yr = a; }
        ringL[rpos] = yl; ringR[rpos] = yr; rpos = (rpos + 1) & (kN - 1);
        key.push(ach > 1 ? 0.5f * (yl + yr) : yl);
        if (++hopCnt >= kHop) {
          hopCnt = 0; fluxFrame();
          if (++tempoFrames >= tempoEvery) { tempoFrames = 0; tempoUpdate(); }
        }
      }
      ++pos;
    }
    cnt.frames += n;
    if (b.flags & AF_TimestampValid) { blockEndNs = b.captureNs + (Ns)((double)n * 1e9 / rate); haveNs = true; }
    else { blockEndNs = (Ns)((double)pos * 1e9 / rate); haveNs = true; }
    srcNs = blockEndNs;
    if (key.updates() != keyUpdSeen) { keyUpdSeen = key.updates(); keyUpdNs = srcNs; }
  }

  void tempoUpdate() {
    if (silent && silentWins * 0.05 >= 2.0) return;
    if (tempo.update((double)pos / rate)) tempoUpdNs = srcNs;
  }

  FeatureSnapshot snapshot(Ns nowNs) {
    FeatureSnapshot f;
    f.sourceNs = srcNs;
    for (int c = 0; c < 2; ++c) { f.rmsDb[c] = rmsDb[c]; f.peakDb[c] = peakDb[c]; }
    f.rmsDbMix = rmsMix;
    for (int b = 0; b < 3; ++b) {
      f.bandDb[b] = db10(bandP[b]);
      f.band[b] = clamp01((f.bandDb[b] + 60.0f) / 60.0f);
    }
    f.onsetStrength = clamp01(onsetStr);
    f.onset = onsetCount > onsetsReported; onsetsReported = onsetCount;
    f.onsetCount = onsetCount;
    f.inputNearFullScale = nearHold > 0 || winNear;
    f.possibleClipping = clipHold > 0 || winClip;
    f.silent = silent;
    f.sequence = ++seq;
    if (tempo.valid()) {
      f.tempo.valid = true; f.tempo.value = tempo.bpm(); f.tempo.confidence = tempo.confidence();
      f.tempo.ageNs = std::max<Ns>(0, nowNs - tempoUpdNs); f.tempo.source = Estimate::Estimated;
    }
    if (key.valid()) {
      f.key.valid = true; f.key.value = key.pitchClass(); f.key.minor = key.minor();
      f.key.confidence = key.confidence(); f.key.ageNs = std::max<Ns>(0, nowNs - keyUpdNs); f.key.source = Estimate::Estimated;
    }
    return f;
  }
};

Analyzer::Analyzer(uint32_t rate, uint32_t channels) : p_(new Impl(rate ? rate : 48000, channels ? channels : 1)) {}
Analyzer::~Analyzer() = default;
void Analyzer::process(const AudioBlock& b) { p_->process(b); }
FeatureSnapshot Analyzer::snapshot(Ns nowNs) { return p_->snapshot(nowNs); }
void Analyzer::reset() { p_->hardReset(); p_->started = false; p_->cnt = AnalyzerCounters(); p_->seq = 0; p_->onsetCount = 0; p_->onsetsReported = 0; }
const AnalyzerCounters& Analyzer::counters() const { return p_->cnt; }
uint32_t Analyzer::rate() const { return p_->rate; }
uint32_t Analyzer::channels() const { return p_->ch; }

} // namespace ss
