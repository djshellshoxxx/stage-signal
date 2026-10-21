#include "ss/audio/analyzer.hpp"
#include <algorithm>
#include <cmath>

namespace ss::audio {

using C = AnalyzerConstants;

namespace {
constexpr double kPi = 3.14159265358979323846;
inline float toDb(double lin) { return lin <= 1e-6 ? -120.f : static_cast<float>(std::max(-120.0, 20.0 * std::log10(lin))); }
}

Status Analyzer::prepare(std::uint32_t rateHz, std::uint32_t channels) {
    ready_ = false;
    if (rateHz < 8000 || rateHz > 192000) return makeError(ErrorCategory::InvalidArgument, "sample rate out of range");
    if (channels < 1 || channels > 2) return makeError(ErrorCategory::UnsupportedCapability, "only mono and stereo are analysed");
    rate_ = rateHz; ch_ = channels;
    subLen_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(rateHz * 0.005)));
    if (!fft_.prepare(C::kFrame)) return makeError(ErrorCategory::InternalFault, "fft");
    for (auto& r : ring_) r.assign(C::kFrame, 0.f);
    win_.assign(C::kFrame, 0.f); a_.assign(C::kFrame, 0.f); b_.assign(C::kFrame, 0.f);
    const std::size_t nb = C::kFrame / 2 + 1;
    pA_.assign(nb, 0.f); pB_.assign(nb, 0.f); pw_.assign(nb, 0.f); prevL_.assign(nb, 0.f);
    double sw = 0.0, sw2 = 0.0;
    for (std::size_t i = 0; i < C::kFrame; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(C::kFrame));
        win_[i] = static_cast<float>(w); sw += w; sw2 += w * w;
    }
    (void)sw;
    // Band value = 2 x one-sided mean-square power => a full-scale sine inside a band reads 1.0.
    bandScale_ = static_cast<float>(4.0 / (double(C::kFrame) * sw2));
    const double binHz = double(rateHz) / double(C::kFrame), nyq = 0.5 * rateHz;
    for (int b = 0; b < 3; ++b) {
        const double lo = C::kBandEdgesHz[b], hi = std::min(C::kBandEdgesHz[b + 1], nyq);
        if (lo >= nyq) { bandLo_[b] = bandHi_[b] = 0; continue; }
        bandLo_[b] = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(lo / binHz)));
        bandHi_[b] = std::min<std::size_t>(C::kFrame / 2 + 1, static_cast<std::size_t>(std::ceil(hi / binHz)));
        if (C::kBandEdgesHz[b + 1] > nyq) bandHi_[b] = C::kFrame / 2 + 1;   // include the Nyquist bin
    }
    fluxHi_ = std::min<std::size_t>(C::kFrame / 2, static_cast<std::size_t>(std::min(12000.0, nyq) / binHz));
    // flux bands: kFluxBands log-spaced groups of FFT bins 1..fluxHi_ (each at least one bin wide)
    fluxEdge_[0] = 1;
    for (std::size_t i = 1; i <= C::kFluxBands; ++i) {
        const std::size_t ideal = static_cast<std::size_t>(std::lround(std::pow(double(fluxHi_), double(i) / C::kFluxBands)));
        fluxEdge_[i] = std::max(fluxEdge_[i - 1] + 1, ideal);
    }
    fluxEdge_[C::kFluxBands] = fluxHi_ + 1;
    for (std::size_t i = C::kFluxBands; i-- > 0;) fluxEdge_[i] = std::min(fluxEdge_[i], fluxEdge_[i + 1] - 1);
    // spectrum display bins
    const double ratio = std::pow(C::kSpectrumHiHz / C::kSpectrumLoHz, 1.0 / C::kSpectrumBins);
    for (std::size_t i = 0; i < C::kSpectrumBins; ++i) {
        const double e0 = C::kSpectrumLoHz * std::pow(ratio, double(i)), e1 = e0 * ratio;
        if (e0 >= nyq) { spLo_[i] = -1; spHi_[i] = -1; spFrac_[i] = 0.f; continue; }
        int lo = std::max(1, static_cast<int>(std::ceil(e0 / binHz)));
        int hi = static_cast<int>(std::ceil(std::min(e1, nyq + binHz) / binHz)) - 1;
        hi = std::min<int>(hi, static_cast<int>(C::kFrame / 2));
        if (hi >= lo) { spLo_[i] = lo; spHi_[i] = hi; spFrac_[i] = 0.f; }
        else {
            const double x = std::sqrt(e0 * e1) / binHz;
            int k0 = std::max(1, static_cast<int>(std::floor(x)));
            k0 = std::min<int>(k0, static_cast<int>(C::kFrame / 2) - 1);
            spLo_[i] = k0; spHi_[i] = -2;                        // -2 => interpolate k0..k0+1
            spFrac_[i] = static_cast<float>(std::clamp(x - k0, 0.0, 1.0));
        }
    }
    refractorySamples_ = static_cast<std::size_t>(C::kRefractoryMs * 1e-3 * rateHz);
    events_.assign(C::kOnsetQueue, OnsetEvent{});
    if (!tempo_.prepare(rateHz, C::kHop)) return makeError(ErrorCategory::InternalFault, "tempo");
    if (!key_.prepare(rateHz)) return makeError(ErrorCategory::InternalFault, "key");
    if (!arc_.prepare(double(rateHz) / double(C::kHop))) return makeError(ErrorCategory::InternalFault, "arc");
    ready_ = true;
    reset();
    return okStatus();
}

void Analyzer::reset() {
    if (!ready_) return;
    subs_.fill(Sub{}); cur_ = Sub{};
    subCount_ = subIdx_ = subFilled_ = 0;
    rmsDb_[0] = rmsDb_[1] = peakDb_[0] = peakDb_[1] = -120.f;
    silent_ = true; nearFs_ = false; clipRun_ = clipRunPublished_ = 0;
    for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.f);
    wpos_ = hopCount_ = 0;
    bands_[0] = bands_[1] = bands_[2] = 0.f;
    spec_.fill(0.f);
    prevValid_ = false;
    fluxHist_.fill(0.f); fluxIdx_ = fluxFilled_ = 0;
    fluxPrev1_ = fluxPrev2_ = curStrength_ = latchedStrength_ = 0.f;
    onsetLatched_ = false; haveOnset_ = false; lastOnsetSample_ = 0; lastOnsetFlux_ = 0.f;
    evHead_ = evCount_ = 0;
    clock_ = expectedNext_ = 0; haveIndex_ = false; timeBaseNs_ = 0;
    tempo_.reset(); key_.reset(); arc_.reset();
    monoN_ = 0;
    stats_ = AnalyzerStats{};
}

void Analyzer::flushDiscontinuity(const AudioBlockView& b) {
    ++stats_.discontinuities;
    if (haveIndex_ && b.firstSampleIndex > expectedNext_) {
        const std::uint64_t gap = b.firstSampleIndex - expectedNext_;
        clock_ += std::min<std::uint64_t>(gap, std::uint64_t(rate_) * 3600);
    }
    // The 1024-sample window would straddle the gap: flush it so the splice cannot create a false onset.
    for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.f);
    hopCount_ = 0; prevValid_ = false;
    fluxPrev1_ = fluxPrev2_ = 0.f;
    cur_ = Sub{}; subCount_ = 0; clipRun_ = 0;
    // tempo/energy history is kept; they only see a short run of zero flux.
}

void Analyzer::process(const AudioBlockView& b) {
    if (!ready_) return;
    ++stats_.blocks;
    if (b.frames == 0) return;
    if (b.rateHz != rate_ || b.channels != ch_) { ++stats_.mismatchedBlocks; return; }
    if (b.flags & kAudioDiscontinuity) flushDiscontinuity(b);
    haveIndex_ = true;
    expectedNext_ = b.firstSampleIndex + b.frames;
    if (b.flags & kAudioTimestampValid) timeBaseNs_ = b.captureNs - samplesToNs(clock_, rate_);
    const bool zeros = (b.flags & kAudioSilent) != 0 || b.interleaved == nullptr;
    const float lim = C::kSanitizeLimit;
    for (std::uint32_t f = 0; f < b.frames; ++f) {
        float x[2] = {0.f, 0.f};
        if (!zeros) {
            for (std::uint32_t c = 0; c < ch_; ++c) {
                float v = b.interleaved[std::size_t(f) * ch_ + c];
                if (!std::isfinite(v)) { v = 0.f; ++stats_.nonFiniteSamples; }
                else if (v > lim) { v = lim; ++stats_.clampedSamples; }
                else if (v < -lim) { v = -lim; ++stats_.clampedSamples; }
                x[c] = v;
            }
        }
        if (ch_ == 1) x[1] = x[0];
        pushFrame(x);
    }
    if (monoN_ > 0) { key_.process(monoStage_.data(), monoN_); monoN_ = 0; }
    stats_.samples += b.frames;
}

void Analyzer::pushFrame(const float* x) {
    constexpr float kNear = 0.94406088f;           // 10^(-0.5/20)
    bool near = false;
    for (std::uint32_t c = 0; c < ch_; ++c) {
        const float a = std::fabs(x[c]);
        cur_.sumsq[c] += double(x[c]) * x[c];
        if (a > cur_.peak[c]) cur_.peak[c] = a;
        if (a >= kNear) near = true;
        ring_[c][wpos_] = x[c];
    }
    if (near) { ++clipRun_; ++stats_.nearFullScaleSamples; if (clipRun_ > cur_.maxRun) cur_.maxRun = clipRun_; }
    else clipRun_ = 0;
    wpos_ = (wpos_ + 1) & (C::kFrame - 1);
    monoStage_[monoN_++] = ch_ == 2 ? 0.5f * (x[0] + x[1]) : x[0];
    if (monoN_ == monoStage_.size()) { key_.process(monoStage_.data(), monoN_); monoN_ = 0; }
    ++clock_;
    if (++subCount_ >= subLen_) finishSub();
    if (++hopCount_ >= C::kHop) { hopCount_ = 0; runFrame(); }
}

void Analyzer::finishSub() {
    subs_[subIdx_] = cur_;
    subIdx_ = (subIdx_ + 1) % C::kSubWindows;
    if (subFilled_ < C::kSubWindows) ++subFilled_;
    cur_ = Sub{}; subCount_ = 0;
    double ss[2] = {0, 0}; float pk[2] = {0, 0}; std::uint32_t run = 0;
    for (std::size_t i = 0; i < subFilled_; ++i) {
        for (int c = 0; c < 2; ++c) { ss[c] += subs_[i].sumsq[c]; pk[c] = std::max(pk[c], subs_[i].peak[c]); }
        run = std::max(run, subs_[i].maxRun);
    }
    const double n = double(subFilled_) * double(subLen_);
    for (std::uint32_t c = 0; c < ch_; ++c) { rmsDb_[c] = toDb(std::sqrt(ss[c] / n)); peakDb_[c] = toDb(pk[c]); }
    if (ch_ == 1) { rmsDb_[1] = rmsDb_[0]; peakDb_[1] = peakDb_[0]; }
    constexpr float kNear = 0.94406088f;           // 10^(-0.5/20)
    nearFs_ = std::max(pk[0], ch_ == 2 ? pk[1] : 0.f) >= kNear;
    clipRunPublished_ = run;
    silent_ = std::max(rmsDb_[0], rmsDb_[1]) < static_cast<float>(C::kSilenceDb);
}

void Analyzer::runFrame() {
    const std::size_t N = C::kFrame, nb = N / 2 + 1;
    const bool st = ch_ == 2;
    for (std::size_t n = 0; n < N; ++n) {
        const std::size_t idx = (wpos_ + n) & (N - 1);
        a_[n] = ring_[0][idx] * win_[n];
        if (st) b_[n] = ring_[1][idx] * win_[n];
    }
    fft_.powerSpectra(a_.data(), st ? b_.data() : nullptr, pA_.data(), pB_.data());
    const float scale = bandScale_ * (st ? 0.5f : 1.f);
    for (std::size_t k = 0; k < nb; ++k) pw_[k] = (st ? pA_[k] + pB_[k] : pA_[k]) * scale;
    pw_[0] = 0.f;                                              // DC excluded everywhere

    float total = 0.f;
    for (int b = 0; b < 3; ++b) {
        float s = 0.f;
        for (std::size_t k = bandLo_[b]; k < bandHi_[b]; ++k) s += pw_[k];
        bands_[b] = s; total += s;
    }
    // spectrum display (instant attack, exponential release)
    for (std::size_t i = 0; i < C::kSpectrumBins; ++i) {
        float v = 0.f;
        if (spLo_[i] >= 0) {
            float p;
            if (spHi_[i] == -2) p = pw_[spLo_[i]] * (1.f - spFrac_[i]) + pw_[spLo_[i] + 1] * spFrac_[i];
            else { float s = 0.f; for (int k = spLo_[i]; k <= spHi_[i]; ++k) s += pw_[k]; p = s; }
            const double db = 10.0 * std::log10(std::max<double>(p, 1e-12));
            v = static_cast<float>(std::clamp((db - C::kSpectrumFloorDb) / (0.0 - C::kSpectrumFloorDb), 0.0, 1.0));
        }
        spec_[i] = std::max(v, spec_[i] * C::kSpectrumRelease);
    }
    // positive spectral flux on log-compressed magnitudes
    float flux = 0.f;
    // Positive log-magnitude differences, averaged within each log-spaced band, then averaged over the bands, so
    // narrow-band onsets (kick drums) are not diluted by the many high-frequency bins.
    for (std::size_t bnd = 0; bnd < C::kFluxBands; ++bnd) {
        float s = 0.f;
        for (std::size_t k = fluxEdge_[bnd]; k < fluxEdge_[bnd + 1]; ++k) {
            const float l = std::log1p(C::kFluxCompression * std::sqrt(pw_[k]));
            const float d = l - prevL_[k];
            if (d > 0.f) s += d;
            prevL_[k] = l;
        }
        flux += s / float(fluxEdge_[bnd + 1] - fluxEdge_[bnd]);
    }
    flux = prevValid_ ? flux / float(C::kFluxBands) : 0.f;
    prevValid_ = true;
    if (!std::isfinite(flux)) flux = 0.f;
    curStrength_ = 1.f - std::exp(-flux / C::kFluxScale);

    // adaptive threshold from the history *before* the candidate frame (the two newest values are excluded)
    float candidate = fluxPrev1_;
    if (fluxFilled_ >= 8) {
        const std::size_t n = std::min(fluxFilled_, C::kFluxHistory) - 2;
        double sum = 0.0, sum2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = fluxHist_[(fluxIdx_ + C::kFluxHistory - 3 - i) % C::kFluxHistory];
            sum += v; sum2 += v * v;
        }
        const double mean = sum / double(n), var = std::max(0.0, sum2 / double(n) - mean * mean);
        const std::uint64_t candEnd = clock_ - C::kHop;                // end of the candidate frame
        float thr = std::max(C::kThresholdFloor, static_cast<float>(mean + C::kThresholdStd * std::sqrt(var)));
        if (haveOnset_) {      // post-onset suppression: decaying floor stops decay/glide bumps of one hit re-triggering
            const double dt = double(candEnd - lastOnsetSample_) / double(rate_);
            thr = std::max(thr, static_cast<float>(C::kPostOnsetRatio * lastOnsetFlux_ * std::exp(-dt / C::kPostOnsetTauS)));
        }
        const bool refrOk = !haveOnset_ || candEnd - lastOnsetSample_ >= refractorySamples_;
        if (candidate > thr && candidate >= fluxPrev2_ && candidate > flux && refrOk) {
            const std::uint64_t onsetSample = candEnd > C::kFrame / 4 ? candEnd - C::kFrame / 4 : 0;
            lastOnsetSample_ = candEnd; haveOnset_ = true; lastOnsetFlux_ = candidate;
            const float strength = 1.f - std::exp(-candidate / C::kFluxScale);
            onsetLatched_ = true; latchedStrength_ = strength;
            ++stats_.onsets;
            const std::size_t tail = (evHead_ + evCount_) % C::kOnsetQueue;
            if (evCount_ == C::kOnsetQueue) { evHead_ = (evHead_ + 1) % C::kOnsetQueue; ++stats_.onsetsDropped; }
            else ++evCount_;
            events_[tail] = OnsetEvent{toNs(onsetSample), strength};
        }
    }
    fluxHist_[fluxIdx_] = flux; fluxIdx_ = (fluxIdx_ + 1) % C::kFluxHistory; if (fluxFilled_ < C::kFluxHistory) ++fluxFilled_;
    fluxPrev2_ = fluxPrev1_; fluxPrev1_ = flux;

    tempo_.push(flux, silent_);
    arc_.push(total, silent_);
}

FeatureSnapshot Analyzer::build(bool fired) const {
    FeatureSnapshot s;
    for (int c = 0; c < 2; ++c) { s.rmsDb[c] = rmsDb_[c]; s.peakDb[c] = peakDb_[c]; }
    for (int b = 0; b < 3; ++b) s.band[b] = bands_[b];
    s.onsetFired = fired;
    s.onsetStrength = fired ? latchedStrength_ : curStrength_;
    s.energyArc = arc_.arc();
    s.arcPhase = static_cast<std::uint8_t>(arc_.phase());
    s.inputNearFullScale = nearFs_;
    s.clipRunLength = clipRunPublished_;
    s.tempo = tempo_.state();
    s.key = key_.state();
    s.sourceNs = toNs(clock_);
    s.silent = silent_ || !ready_;
    return s;
}

FeatureSnapshot Analyzer::snapshot() {
    FeatureSnapshot s = build(onsetLatched_);
    onsetLatched_ = false;
    return s;
}

void Analyzer::copySpectrum(float* out) const {
    if (!out) return;
    for (std::size_t i = 0; i < C::kSpectrumBins; ++i) out[i] = spec_[i];
}

void Analyzer::copyWaveform(float* out) const {
    if (!out) return;
    if (!ready_) { std::fill(out, out + C::kWaveformPoints, 0.f); return; }
    const std::size_t N = C::kFrame, g = N / C::kWaveformPoints;
    for (std::size_t j = 0; j < C::kWaveformPoints; ++j) {
        float s = 0.f;
        for (std::size_t m = 0; m < g; ++m) {
            const std::size_t idx = (wpos_ + j * g + m) & (N - 1);
            s += ch_ == 2 ? 0.5f * (ring_[0][idx] + ring_[1][idx]) : ring_[0][idx];
        }
        out[j] = std::clamp(s / float(g), -1.f, 1.f);
    }
}

std::size_t Analyzer::drainOnsets(OnsetEvent* out, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && evCount_ > 0) {
        out[n++] = events_[evHead_];
        evHead_ = (evHead_ + 1) % C::kOnsetQueue; --evCount_;
    }
    return n;
}

}  // namespace ss::audio
