#include "ss/audio/tempo.hpp"
#include <algorithm>
#include <cmath>

namespace ss::audio {

using L = TempoLimits;

bool TempoEstimator::prepare(std::uint32_t rateHz, std::uint32_t hopSamples) {
    if (rateHz < 8000 || rateHz > 192000 || hopSamples == 0 || hopSamples > rateHz) return false;
    rate_ = rateHz; hop_ = hopSamples;
    frameRate_ = static_cast<double>(rateHz) / hopSamples;
    W_ = static_cast<std::size_t>(std::ceil(L::kWindowS * frameRate_));
    minFrames_ = static_cast<std::size_t>(L::kMinEvidenceS * frameRate_);
    updateFrames_ = std::max<std::size_t>(1, static_cast<std::size_t>(L::kUpdateS * frameRate_));
    silenceFrames_ = static_cast<std::size_t>(L::kSilenceHoldS * frameRate_);
    staleFrames_ = static_cast<std::size_t>(L::kStaleS * frameRate_);
    lagLo_ = std::max<std::size_t>(2, static_cast<std::size_t>(std::floor(60.0 * frameRate_ / L::kMaxBpm)));
    lagHi_ = static_cast<std::size_t>(std::ceil(60.0 * frameRate_ / L::kMinBpm));
    if (lagHi_ + 2 >= W_ / 2) return false;
    const std::size_t nLags = lagHi_ - lagLo_ + 3;
    perFrame_ = std::max<std::size_t>(1, (nLags + updateFrames_ / 2 - 1) / std::max<std::size_t>(1, updateFrames_ / 2));
    meanA_ = 1.0 - std::exp(-1.0 / (0.4 * frameRate_));
    buf_.assign(W_, 0.f); work_.assign(W_, 0.f); r_.assign(lagHi_ + 3, 0.f);
    reset();
    return true;
}

void TempoEstimator::clearEstimate() {
    valid_ = false; bpm_ = conf_ = 0.0; candCount_ = 0; candBpm_ = candConf_ = 0.0; pend_ = false; pendBpm_ = 0.0;
}

void TempoEstimator::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.f);
    w_ = filled_ = cursor_ = n_ = 0;
    cycling_ = false;
    frame_ = sinceCycle_ = silentRun_ = 0;
    runMean_ = 0.0;
    lastUpdateFrame_ = lastGoodFrame_ = 0;
    clearEstimate();
}

void TempoEstimator::push(float v, bool silent) {
    if (W_ == 0) return;
    ++frame_;
    if (!std::isfinite(v) || v < 0.f) v = 0.f;
    if (silent) { v = 0.f; ++silentRun_; } else silentRun_ = 0;
    if (silentRun_ >= silenceFrames_ && silenceFrames_ > 0) {        // silence => Unknown and forget history
        if (valid_ || filled_ > 0) {
            clearEstimate();
            std::fill(buf_.begin(), buf_.end(), 0.f);
            w_ = filled_ = 0; cycling_ = false; runMean_ = 0.0; sinceCycle_ = 0;
        }
        return;
    }
    // local-mean subtracted, half-wave rectified onset envelope
    runMean_ += meanA_ * (static_cast<double>(v) - runMean_);
    const double e = std::max(0.0, static_cast<double>(v) - runMean_);
    buf_[w_] = static_cast<float>(e);
    w_ = (w_ + 1) % W_;
    if (filled_ < W_) ++filled_;
    ++sinceCycle_;
    if (!cycling_ && sinceCycle_ >= updateFrames_ && filled_ >= minFrames_) startCycle();
    if (cycling_) stepCycle();
    if (valid_ && frame_ - lastGoodFrame_ > staleFrames_) clearEstimate();
}

void TempoEstimator::startCycle() {
    n_ = filled_;
    const std::size_t start = (w_ + W_ - n_) % W_;
    // 5-tap Hann smoothing removes the dependence of the autocorrelation peak height on hop alignment.
    static constexpr float kSm[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    float peak = 0.f;
    for (std::size_t i = 0; i < n_; ++i) peak = std::max(peak, buf_[(start + i) % W_]);
    double mean = 0.0;
    for (std::size_t i = 0; i < n_; ++i) {
        float acc = 0.f;
        for (int j = -2; j <= 2; ++j) {
            const std::ptrdiff_t q = static_cast<std::ptrdiff_t>(i) + j;
            if (q >= 0 && q < static_cast<std::ptrdiff_t>(n_)) acc += kSm[j + 2] * buf_[(start + static_cast<std::size_t>(q)) % W_];
        }
        work_[i] = acc; mean += acc;
    }
    mean /= static_cast<double>(n_);
    var_ = 0.0;
    for (std::size_t i = 0; i < n_; ++i) { work_[i] = static_cast<float>(work_[i] - mean); var_ += double(work_[i]) * work_[i]; }
    var_ = peak >= static_cast<float>(L::kMinEnvelopePeak) ? var_ / static_cast<double>(n_) : 0.0;   // no onsets worth a tempo
    cursor_ = lagLo_ - 1;
    cycling_ = true;
}

void TempoEstimator::stepCycle() {
    const std::size_t last = lagHi_ + 1;
    for (std::size_t k = 0; k < perFrame_ && cursor_ <= last; ++k, ++cursor_) {
        const std::size_t lag = cursor_;
        double s = 0.0;
        for (std::size_t i = 0; i + lag < n_; ++i) s += double(work_[i]) * work_[i + lag];
        r_[lag] = (var_ > 1e-18 && n_ > lag) ? static_cast<float>(s / double(n_ - lag) / var_) : 0.f;
    }
    if (cursor_ > last) { cycling_ = false; sinceCycle_ = 0; finalise(); }
}

void TempoEstimator::finalise() {
    if (var_ <= 1e-12) { if (valid_) { /* no rhythmic energy: let staleness expire the estimate */ } return; }
    auto prior = [&](std::size_t lag) {
        const double o = std::log2(lagToBpm(double(lag)) / 120.0) / 1.5;      // mild log-normal prior around 120 BPM
        return std::exp(-0.5 * o * o);
    };
    std::size_t best = 0; double bestV = -1e9;
    for (std::size_t l = lagLo_; l <= lagHi_; ++l) {
        if (!(r_[l] >= r_[l - 1] && r_[l] > r_[l + 1])) continue;          // local maxima only
        const double v = r_[l] * prior(l);
        if (v > bestV) { bestV = v; best = l; }
    }
    auto refine = [&](std::size_t p) {
        const double y0 = r_[p - 1], y1 = r_[p], y2 = r_[p + 1], d = y0 - 2 * y1 + y2;
        double delta = std::fabs(d) > 1e-12 ? 0.5 * (y0 - y2) / d : 0.0;
        return double(p) + std::clamp(delta, -0.5, 0.5);
    };
    double conf = 0.0, bpm = 0.0;
    if (best != 0 && r_[best] > 0.f) {
        std::size_t pk = best;
        const double r0 = r_[best];
        // octave resolution: prefer the fastest related tempo (k x) that is also a clear peak and <= preferred max
        for (int k = 4; k >= 2; --k) {
            const double lc = double(best) / k;
            if (lc < double(lagLo_) || lagToBpm(lc) > L::kPreferredMaxBpm) continue;
            const std::size_t li = static_cast<std::size_t>(lc + 0.5);
            const std::size_t w = std::max<std::size_t>(2, static_cast<std::size_t>(0.03 * double(li)));
            std::size_t bp = 0; float bv = -1e9f;
            for (std::size_t l = li > w ? li - w : lagLo_; l <= li + w && l <= lagHi_; ++l) {
                if (l < lagLo_) continue;
                if (r_[l] >= r_[l - 1] && r_[l] > r_[l + 1] && r_[l] > bv) { bv = r_[l]; bp = l; }
            }
            if (bp != 0 && bv >= L::kOctaveAccept * r0) { pk = bp; break; }
        }
        bpm = lagToBpm(refine(pk));
        conf = std::clamp((double(r_[pk]) - L::kConfR0) / L::kConfSpan, 0.0, 1.0);
    }
    if (bpm < L::kMinBpm * 0.97 || bpm > L::kMaxBpm * 1.03) conf = 0.0;
    if (conf < L::kMinConfidence) return;                                   // rejected; staleness handles expiry
    lastGoodFrame_ = frame_;
    auto agree = [](double a, double b) { return std::fabs(a / b - 1.0) <= L::kAgreeRatio; };
    if (!valid_) {
        if (pend_ && agree(pendBpm_, bpm)) {
            valid_ = true; bpm_ = 0.5 * (pendBpm_ + bpm); conf_ = conf; lastUpdateFrame_ = frame_;
            pend_ = false; candCount_ = 0;
        } else { pend_ = true; pendBpm_ = bpm; }
        return;
    }
    if (agree(bpm_, bpm)) {
        bpm_ += 0.25 * (bpm - bpm_);
        conf_ += 0.3 * (conf - conf_);
        lastUpdateFrame_ = frame_; candCount_ = 0;
    } else {
        if (candCount_ > 0 && agree(candBpm_, bpm)) { ++candCount_; candBpm_ = 0.5 * (candBpm_ + bpm); candConf_ = conf; }
        else { candCount_ = 1; candBpm_ = bpm; candConf_ = conf; }
        conf_ *= 0.9;                                                       // disagreement lowers confidence
        if (candCount_ >= L::kSwitchCount) {
            bpm_ = candBpm_; conf_ = candConf_ * 0.6; lastUpdateFrame_ = frame_; candCount_ = 0;
        }
    }
}

TempoState TempoEstimator::state() const {
    TempoState t;
    if (!valid_ || bpm_ <= 0.0) return t;
    t.bpm = bpm_; t.confidence = static_cast<float>(std::clamp(conf_, 0.0, 1.0));
    t.ageNs = frameNs(frame_) - frameNs(lastUpdateFrame_);
    t.source = ValueSource::Estimated;
    return t;
}

// ---------------------------------------------------------------------------------------------------------
TempoController::TempoController(TempoControllerConfig cfg) : cfg_(cfg) {
    cfg_.maxTaps = std::clamp(cfg_.maxTaps, 2, static_cast<int>(taps_.size()));
}

bool TempoController::tap(Ns nowNs) {
    if (nTaps_ > 0 && (nowNs - taps_[nTaps_ - 1] > cfg_.tapResetGap || nowNs <= taps_[nTaps_ - 1])) nTaps_ = 0;
    if (nTaps_ == cfg_.maxTaps) { for (int i = 1; i < nTaps_; ++i) taps_[i - 1] = taps_[i]; --nTaps_; }
    taps_[nTaps_++] = nowNs;
    if (nTaps_ < 2) return false;
    const double meanIntervalS = nsToSec(taps_[nTaps_ - 1] - taps_[0]) / double(nTaps_ - 1);
    const double bpm = 60.0 / meanIntervalS;
    if (!(bpm >= cfg_.minBpm && bpm <= cfg_.maxBpm)) return false;
    bpm_ = bpm; src_ = ValueSource::Tap; setNs_ = nowNs;
    return true;
}

Status TempoController::setManual(double bpm, Ns nowNs) {
    if (!std::isfinite(bpm) || bpm < cfg_.minBpm || bpm > cfg_.maxBpm)
        return makeError(ErrorCategory::InvalidArgument, "manual BPM out of range");
    bpm_ = bpm; src_ = ValueSource::Manual; setNs_ = nowNs; nTaps_ = 0;
    return okStatus();
}

void TempoController::release() { src_ = ValueSource::Unknown; nTaps_ = 0; bpm_ = 0.0; }

TempoState TempoController::resolve(const TempoState& est, Ns nowNs) const {
    if (src_ == ValueSource::Unknown) return est;
    TempoState t;
    t.bpm = bpm_; t.confidence = 1.f; t.source = src_;
    t.ageNs = nowNs > setNs_ ? nowNs - setNs_ : 0;
    return t;
}

}  // namespace ss::audio
