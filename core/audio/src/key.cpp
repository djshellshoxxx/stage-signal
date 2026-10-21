#include "ss/audio/key.hpp"
#include <algorithm>
#include <cmath>

namespace ss::audio {

using K = KeyLimits;

const std::array<double, 12>& KeyEstimator::majorProfile() {
    static const std::array<double, 12> p = {5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0};
    return p;
}
const std::array<double, 12>& KeyEstimator::minorProfile() {
    static const std::array<double, 12> p = {5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0};
    return p;
}

bool KeyEstimator::prepare(std::uint32_t rateHz) {
    if (rateHz < 8000 || rateHz > 192000) return false;
    rate_ = rateHz;
    decim_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(rateHz / K::kDecimatedRateHz)));
    fsd_ = static_cast<double>(rateHz) / decim_;
    // Butterworth 4th order = two biquads with Q 0.5412 and 1.3066, cut-off 0.425 * decimated rate.
    const double pi = 3.14159265358979323846;
    const double fc = 0.425 * fsd_, w0 = 2.0 * pi * fc / rateHz;
    const double qs[2] = {0.54119610, 1.30656296};
    for (int i = 0; i < 2; ++i) {
        const double alpha = std::sin(w0) / (2.0 * qs[i]), c = std::cos(w0), a0 = 1.0 + alpha;
        lp_[i] = Bq{};
        lp_[i].b0 = static_cast<float>((1.0 - c) / 2.0 / a0);
        lp_[i].b1 = static_cast<float>((1.0 - c) / a0);
        lp_[i].b2 = lp_[i].b0;
        lp_[i].a1 = static_cast<float>(-2.0 * c / a0);
        lp_[i].a2 = static_cast<float>((1.0 - alpha) / a0);
    }
    if (!fft_.prepare(K::kFrame)) return false;
    ring_.assign(K::kFrame, 0.f); fbuf_.assign(K::kFrame, 0.f); win_.assign(K::kFrame, 0.f);
    pow_.assign(K::kFrame / 2 + 1, 0.f);
    for (std::size_t i = 0; i < K::kFrame; ++i)
        win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * double(i) / double(K::kFrame)));
    kLo_ = static_cast<std::size_t>(std::ceil(K::kMinFreqHz * K::kFrame / fsd_));
    kHi_ = std::min<std::size_t>(static_cast<std::size_t>(K::kMaxFreqHz * K::kFrame / fsd_), K::kFrame / 2 - 2);
    reset();
    return true;
}

void KeyEstimator::reset() {
    for (auto& b : lp_) { b.z1 = b.z2 = 0.f; }
    decimPhase_ = 0; wpos_ = decCount_ = hopCount_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.f);
    for (auto& c : chroma_) c.fill(0.f);
    active_.fill(false);
    slot_ = framesSinceEval_ = framesTotal_ = 0;
    inSamples_ = 0;
    tonic_ = -1; minor_ = false; conf_ = 0.0; confirmNs_ = 0;
    candTonic_ = -1; candRun_ = 0;
    diag_ = KeyDiagnostics{};
}

void KeyEstimator::process(const float* mono, std::size_t n) {
    if (rate_ == 0) return;
    for (std::size_t i = 0; i < n; ++i) {
        float x = mono ? mono[i] : 0.f;
        if (!std::isfinite(x)) x = 0.f;
        x = std::clamp(x, -64.f, 64.f);
        const float y = lp_[1].run(lp_[0].run(x));
        ++inSamples_;
        if (++decimPhase_ < decim_) continue;
        decimPhase_ = 0;
        ring_[wpos_] = y; wpos_ = (wpos_ + 1) % K::kFrame;
        ++decCount_;
        if (++hopCount_ >= K::kHop) {
            hopCount_ = 0;
            if (decCount_ >= K::kFrame) frame();
        }
    }
}

void KeyEstimator::frame() {
    // linearise + window
    double e2 = 0.0;
    for (std::size_t i = 0; i < K::kFrame; ++i) {
        const float s = ring_[(wpos_ + i) % K::kFrame];
        e2 += double(s) * s;
        fbuf_[i] = s * win_[i];
    }
    const float rms = static_cast<float>(std::sqrt(e2 / double(K::kFrame)));
    auto& ch = chroma_[slot_];
    ch.fill(0.f);
    bool act = false;
    if (rms >= K::kActiveRms) {
        fft_.powerSpectra(fbuf_.data(), nullptr, pow_.data(), nullptr);
        float mx = 0.f;
        for (std::size_t k = kLo_; k <= kHi_; ++k) mx = std::max(mx, pow_[k]);
        if (mx > 0.f) {
            const float thr = mx * 0.0025f;           // power: -26 dB relative to the strongest peak (amplitude 0.05)
            const double binHz = fsd_ / double(K::kFrame);
            double sum = 0.0;
            for (std::size_t k = kLo_; k <= kHi_; ++k) {
                const float p = pow_[k];
                if (p < thr || !(p > pow_[k - 1] && p >= pow_[k + 1])) continue;
                const double a = std::log(double(pow_[k - 1]) + 1e-30), b = std::log(double(p)), c = std::log(double(pow_[k + 1]) + 1e-30);
                const double d = a - 2.0 * b + c;
                const double delta = std::fabs(d) > 1e-12 ? std::clamp(0.5 * (a - c) / d, -0.5, 0.5) : 0.0;
                const double f = (double(k) + delta) * binHz;
                const double midi = 69.0 + 12.0 * std::log2(f / 440.0);
                int b36 = static_cast<int>(std::lround(3.0 * midi)) % 36;
                if (b36 < 0) b36 += 36;
                const double w = std::sqrt(double(p));
                ch[b36] += static_cast<float>(w); sum += w;
            }
            if (sum > 0.0) { for (auto& v : ch) v = static_cast<float>(v / sum); act = true; }
        }
    }
    active_[slot_] = act;
    slot_ = (slot_ + 1) % K::kRingFrames;
    ++framesTotal_;
    if (++framesSinceEval_ >= K::kEvalEveryFrames) { framesSinceEval_ = 0; evaluate(); }
}

void KeyEstimator::evaluate() {
    const Ns now = nowNs();
    std::array<double, 36> c36{};
    std::size_t act = 0;
    for (std::size_t s = 0; s < K::kRingFrames; ++s) {
        if (!active_[s]) continue;
        ++act;
        for (int i = 0; i < 36; ++i) c36[i] += chroma_[s][i];
    }
    diag_ = KeyDiagnostics{};
    diag_.activeFrames = act;
    auto decay = [&](double f) {
        conf_ *= f; candRun_ = 0; candTonic_ = -1;
        if (conf_ < K::kMinConfidence) { tonic_ = -1; conf_ = 0.0; }
    };
    if (act < K::kMinActiveFrames) { decay(0.5); return; }

    // tuning: choose the third-semitone offset whose 12 pitch-class centres hold the most energy
    int bestT = 0; double bestE = -1.0;
    for (int t : {0, -1, 1}) {
        double e = 0.0;
        for (int pc = 0; pc < 12; ++pc) e += c36[(3 * pc + t + 72) % 36];
        if (e > bestE + 1e-9) { bestE = e; bestT = t; }
    }
    diag_.tuningOffset = bestT;
    std::array<double, 12> c12{};
    for (int pc = 0; pc < 12; ++pc) for (int j = -1; j <= 1; ++j) c12[pc] += c36[(3 * pc + bestT + j + 72) % 36];

    double mean = 0.0; for (double v : c12) mean += v; mean /= 12.0;
    double var = 0.0; for (double v : c12) var += (v - mean) * (v - mean);
    double r1 = -2, r2 = -2; int t1 = -1; bool m1 = false;
    if (var > 1e-12) {
        for (int minor = 0; minor < 2; ++minor) {
            const auto& prof = minor ? minorProfile() : majorProfile();
            double pm = 0.0; for (double v : prof) pm += v; pm /= 12.0;
            double pv = 0.0; for (double v : prof) pv += (v - pm) * (v - pm);
            for (int tonic = 0; tonic < 12; ++tonic) {
                double cov = 0.0;
                for (int pc = 0; pc < 12; ++pc) cov += (c12[(pc + tonic) % 12] - mean) * (prof[pc] - pm);
                const double r = cov / std::sqrt(var * pv);
                if (r > r1) { r2 = r1; r1 = r; t1 = tonic; m1 = minor != 0; }
                else if (r > r2) r2 = r;
            }
        }
    }
    diag_.bestR = r1; diag_.secondR = r2; diag_.bestTonic = t1; diag_.bestMinor = m1;
    // Confidence = absolute fit (Pearson r of the best template, 0 at r<=0.70, 1 at r>=0.90)
    //              x ambiguity margin (best minus second best, 0 at <=0.02, 1 at >=0.10).
    double cc = 0.0;
    if (t1 >= 0) {
        const double fit = std::clamp((r1 - 0.70) / 0.20, 0.0, 1.0);
        const double margin = std::clamp((r1 - r2 - 0.02) / 0.08, 0.0, 1.0);
        cc = fit * margin;
    }
    diag_.candidateConfidence = cc;

    if (cc < K::kMinConfidence) { decay(0.6); return; }          // low confidence is suppressed, old claim fades
    if (tonic_ < 0) {
        if (candRun_ > 0 && candTonic_ == t1 && candMinor_ == m1) ++candRun_; else { candTonic_ = t1; candMinor_ = m1; candRun_ = 1; }
        if (candRun_ >= 2) { tonic_ = t1; minor_ = m1; conf_ = cc; confirmNs_ = now; candRun_ = 0; }
        return;
    }
    if (t1 == tonic_ && m1 == minor_) {
        conf_ += 0.4 * (cc - conf_); confirmNs_ = now; candRun_ = 0; candTonic_ = -1;
        return;
    }
    // different key: fade the old claim, switch only after two consecutive agreeing evaluations
    if (candRun_ > 0 && candTonic_ == t1 && candMinor_ == m1) ++candRun_; else { candTonic_ = t1; candMinor_ = m1; candRun_ = 1; }
    conf_ *= 0.6;
    if (candRun_ >= 2) { tonic_ = t1; minor_ = m1; conf_ = std::max(cc * 0.7, double(K::kMinConfidence)); confirmNs_ = now; candRun_ = 0; }
    else if (conf_ < K::kMinConfidence) { tonic_ = -1; conf_ = 0.0; }
}

KeyState KeyEstimator::state() const {
    KeyState k;
    if (tonic_ < 0 || conf_ < K::kMinConfidence) return k;
    k.tonic = tonic_; k.minor = minor_; k.confidence = static_cast<float>(std::clamp(conf_, 0.0, 1.0));
    k.ageNs = std::max<Ns>(0, nowNs() - confirmNs_);
    k.source = ValueSource::Estimated;
    return k;
}

// ---------------------------------------------------------------------------------------------------------
Status KeyController::setManual(int tonic, bool minor, Ns nowNs) {
    if (tonic < 0 || tonic > 11) return makeError(ErrorCategory::InvalidArgument, "tonic must be 0..11");
    manual_ = true; tonic_ = tonic; minor_ = minor; setNs_ = nowNs;
    return okStatus();
}
void KeyController::release() { manual_ = false; tonic_ = -1; minor_ = false; }
KeyState KeyController::resolve(const KeyState& est, Ns nowNs) const {
    if (!manual_) return est;
    KeyState k;
    k.tonic = tonic_; k.minor = minor_; k.confidence = 1.f; k.source = ValueSource::Manual;
    k.ageNs = nowNs > setNs_ ? nowNs - setNs_ : 0;
    return k;
}

}  // namespace ss::audio
