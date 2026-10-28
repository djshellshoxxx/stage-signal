#include "ss/dynamics/processor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "ss/dynamics/sliding_extreme.hpp"
#include "ss/dynamics/truepeak.hpp"

namespace ss::dynamics {

namespace {

constexpr float kLn10Over20 = 0.11512925464970229f;   // ln(10)/20
constexpr float kMaxAbsInput = 1.0e6f;                 // +120 dBFS: finite garbage is clamped, not propagated
constexpr float kNearFullScaleLin = 0.94406087628592f; // 10^(-0.5/20)
constexpr double kDetectorWindowMs = 4.0;              // peak-hold window of the compressor detector
constexpr double kParamSmoothMs = 5.0;
constexpr double kBypassRampMs = 10.0;

inline float dbToLin(float db) { return std::exp(db * kLn10Over20); }
inline float linToDb(float lin) { return lin > 1.0e-6f ? 20.0f * std::log10(lin) : kSilenceDb; }

struct Range { float lo, hi, def; std::uint32_t bit; };
constexpr Range kTrim{-24.0f, 12.0f, 0.0f, kPTrim};
constexpr Range kThr{-36.0f, 0.0f, -12.0f, kPThreshold};
constexpr Range kRatio{1.0f, 10.0f, 3.0f, kPRatio};
constexpr Range kAtt{0.5f, 100.0f, 10.0f, kPAttack};
constexpr Range kRel{20.0f, 2000.0f, 150.0f, kPRelease};
constexpr Range kKnee{0.0f, 12.0f, 6.0f, kPKnee};
constexpr Range kMake{0.0f, 12.0f, 0.0f, kPMakeup};
constexpr Range kCeil{-12.0f, -0.5f, -1.0f, kPCeiling};
constexpr Range kLRel{20.0f, 500.0f, 100.0f, kPLimiterRelease};
// Safety margin under the dBTP ceiling. The 8x/32-tap detector reads a few tenths of a dB low on loud broadband noise (measured
// against a 129-tap reference, see docs/specs/dynamics.md), so the limiter aims this far below the set ceiling: the ceiling is an
// upper bound, and tonal material lands about this much under it. Tuned on the synthetic suite in test_truepeak_limiter.cpp.
constexpr float kTruePeakMarginDb = 0.45f;

void clampOne(float& v, const Range& r, std::uint32_t& mask) {
    if (!std::isfinite(v)) { v = r.def; mask |= r.bit; return; }
    if (v < r.lo) { v = r.lo; mask |= r.bit; }
    else if (v > r.hi) { v = r.hi; mask |= r.bit; }
}

struct Sm {
    float cur = 0.0f, tgt = 0.0f;
    void snap(float v) { cur = tgt = v; }
    bool step(float a) {
        if (cur == tgt) return false;
        cur = tgt + (cur - tgt) * a;
        if (std::fabs(cur - tgt) < 1.0e-5f) cur = tgt;
        return true;
    }
};

// Soft-knee static curve: gain reduction in dB (>= 0) for a peak level in dB.
inline float gainReductionDb(float levelDb, float thr, float ratio, float knee) {
    const float over = levelDb - thr;
    const float slope = 1.0f - 1.0f / ratio;
    if (knee <= 0.0f) return over > 0.0f ? over * slope : 0.0f;
    if (2.0f * over < -knee) return 0.0f;
    if (2.0f * std::fabs(over) <= knee) {
        const float x = over + 0.5f * knee;
        return slope * x * x / (2.0f * knee);
    }
    return over * slope;
}

struct Hold {
    float inPeak = 0, outPeak = 0, outTp = 0, compGrMax = 0, limGrMaxLin = 1.0f;
    std::uint32_t clipRunMax = 0;
    std::uint64_t nfsSamples = 0, overSamples = 0, limSamples = 0;
};

// One lookahead limiter stage: min-hold of the required gain over the lookahead window, release-smoothed, then box-averaged so the
// gain ramps instead of stepping. With `tp` the required gain comes from an 8x oversampled detector (reconstructed peak) and a
// minimum over every region that can touch one output sample, and the signal it protects must be delayed by delay() instead of L.
struct LimiterStage {
    bool tp = false;
    std::uint32_t L = 1, N = 2, extra = 0;
    SlidingExtreme<false> gainMin, regionMin;
    TruePeakDetector8 det[2];
    double env = 1.0, boxSum = 0.0;
    std::vector<float> envRing;
    std::uint32_t envPos = 0;
    float marginLin = 1.0f;

    void init(std::uint32_t lookaheadFrames, bool truePeak, float marginDb) {
        tp = truePeak;
        L = std::max<std::uint32_t>(1, lookaheadFrames);
        N = L + 1;
        extra = tp ? kTruePeakExtraDelay : 0u;
        gainMin.init(N);
        envRing.assign(N, 1.0f);
        boxSum = static_cast<double>(N);
        if (tp) { regionMin.init(TruePeakDetector8::kTapsPerPhase); marginLin = dbToLin(-marginDb); }
    }
    std::uint32_t delay() const { return L + extra; }

    // `s`: the stage's input sample for each of `ch` channels. Returns the gain for the sample that leaves the delay line now.
    float step(const float* s, std::uint32_t ch, float ceilLin, double relA) {
        float target;
        if (tp) {
            // The detector reports region r (between samples r and r+1) 16 samples late and an output sample is touched by 32
            // regions, so the required gain is the minimum over the last 32 reports, and the protected signal is delayed by
            // `extra` more samples. The margin covers the spread of the gain across those samples.
            float tpk = 0.0f;
            for (std::uint32_t c = 0; c < ch; ++c) tpk = std::max(tpk, det[c].push(s[c]));
            const float ceilEff = ceilLin * marginLin;
            target = regionMin.push(tpk > ceilEff ? ceilEff / tpk : 1.0f);
        } else {
            float pk = 0.0f;
            for (std::uint32_t c = 0; c < ch; ++c) pk = std::max(pk, std::fabs(s[c]));
            target = pk > ceilLin ? ceilLin / pk : 1.0f;        // sample-peak limiter
        }
        const float m = gainMin.push(target);                    // min over the last N=L+1 targets
        env = (m < env) ? static_cast<double>(m) : static_cast<double>(m) - (static_cast<double>(m) - env) * relA;
        boxSum += env - static_cast<double>(envRing[envPos]);
        envRing[envPos] = static_cast<float>(env);
        if (++envPos == N) {                                     // exact re-sum once per window: no drift
            envPos = 0;
            double sum = 0.0;
            for (std::uint32_t k = 0; k < N; ++k) sum += envRing[k];
            boxSum = sum;
        }
        float g = static_cast<float>(boxSum / N);
        if (!(g <= 1.0f)) g = 1.0f;
        if (g < 0.0f) g = 0.0f;
        return g;
    }
};

}  // namespace

// ---- Engine: all allocation happens in the constructor ---------------------------------------------
struct DynamicsProcessor::Engine {
    DynamicsConfig cfg;
    PrepareInfo info;
    std::uint32_t inCh = 2, outCh = 2, L = 240, N = 241;
    float paramA = 0;   // smoothing coefficient

    // smoothed parameters
    Sm trim, thr, ratio, knee, makeup, ceil, bypass;
    float trimLin = 1, makeupLin = 1, ceilLin = 1;
    float bypassStep = 0;

    // compressor
    SlidingExtreme<true> detMax;
    float grState = 0.0f;
    // limiter (sample-peak, or true-peak when cfg.truePeakLimiter)
    LimiterStage lim;
    std::vector<float> wetRing, dryRing;   // [d * outCh], d = lim.delay()
    std::uint32_t ringPos = 0, d = 1;
    // meters
    TruePeakMeter tp[2];
    std::uint32_t runLen[2] = {0, 0};
    Hold hold;
    std::uint64_t sanitized = 0, frames = 0;
    bool limiterActiveLast = false;
    float compGrCur = 0, limGrCur = 0;

    Engine(const DynamicsConfig& c, const DynamicsParams& p) : cfg(c) {
        inCh = c.inChannels;
        outCh = (inCh == 1 && !c.monoToStereo) ? 1u : 2u;
        L = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(c.lookaheadMs * 1e-3 * c.sampleRateHz)));
        N = L + 1;
        const double fsr = c.sampleRateHz;
        lim.init(L, c.truePeakLimiter, kTruePeakMarginDb);
        d = lim.delay();
        info = {c.sampleRateHz, inCh, outCh, d};
        paramA = static_cast<float>(std::exp(-1.0 / (kParamSmoothMs * 1e-3 * fsr)));
        bypassStep = static_cast<float>(1.0 / (kBypassRampMs * 1e-3 * fsr));
        detMax.init(std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kDetectorWindowMs * 1e-3 * fsr))));
        wetRing.assign(static_cast<std::size_t>(d) * outCh, 0.0f);          // read-then-write on a ring of d frames delays by exactly d
        dryRing.assign(static_cast<std::size_t>(d) * outCh, 0.0f);
        snapTo(p);
    }

    void snapTo(const DynamicsParams& p) {
        trim.snap(p.trimDb); thr.snap(p.thresholdDb); ratio.snap(p.ratio); knee.snap(p.kneeDb);
        makeup.snap(p.makeupDb); ceil.snap(p.ceilingDb); bypass.snap(p.bypass ? 1.0f : 0.0f);
        trimLin = dbToLin(trim.cur); makeupLin = dbToLin(makeup.cur); ceilLin = dbToLin(ceil.cur);
        setTargets(p);
    }

    void setTargets(const DynamicsParams& p) {
        trim.tgt = p.trimDb; thr.tgt = p.thresholdDb; ratio.tgt = p.ratio; knee.tgt = p.kneeDb;
        makeup.tgt = p.makeupDb; ceil.tgt = p.ceilingDb; bypass.tgt = p.bypass ? 1.0f : 0.0f;
    }

    void resetHolds() { hold = Hold{}; runLen[0] = runLen[1] = 0; }

    void process(const float* in, float* out, std::uint32_t nFrames, const DynamicsParams& p);
};

void DynamicsProcessor::Engine::process(const float* in, float* out, std::uint32_t nFrames, const DynamicsParams& p) {
    setTargets(p);
    const double fs = cfg.sampleRateHz;
    const float attA = static_cast<float>(std::exp(-1.0 / (p.attackMs * 1e-3 * fs)));
    const float relA = static_cast<float>(std::exp(-1.0 / (p.releaseMs * 1e-3 * fs)));
    const float limRelA = static_cast<float>(std::exp(-1.0 / (p.limiterReleaseMs * 1e-3 * fs)));
    const bool wantTp = cfg.truePeakMeter;

    float blockCompGrMax = 0.0f, blockLimMinGain = 1.0f;
    std::uint64_t blockLimSamples = 0;

    for (std::uint32_t i = 0; i < nFrames; ++i) {
        // ---- input: sanitise, raw meters, route -------------------------------------------------
        float x[2] = {0.0f, 0.0f};
        for (std::uint32_t c = 0; c < inCh; ++c) {
            float v = in[static_cast<std::size_t>(i) * inCh + c];
            if (!std::isfinite(v)) { v = 0.0f; ++sanitized; }
            else if (v > kMaxAbsInput) v = kMaxAbsInput;
            else if (v < -kMaxAbsInput) v = -kMaxAbsInput;
            x[c] = v;
            const float a = std::fabs(v);
            if (a > hold.inPeak) hold.inPeak = a;
            if (a >= kNearFullScaleLin) {
                ++hold.nfsSamples;
                if (++runLen[c] > hold.clipRunMax) hold.clipRunMax = runLen[c];
            } else {
                runLen[c] = 0;
            }
        }
        if (inCh == 1 && outCh == 2) x[1] = x[0];

        // ---- smoothed parameters ------------------------------------------------------------------
        if (trim.step(paramA)) trimLin = dbToLin(trim.cur);
        if (makeup.step(paramA)) makeupLin = dbToLin(makeup.cur);
        if (ceil.step(paramA)) ceilLin = dbToLin(ceil.cur);
        thr.step(paramA); ratio.step(paramA); knee.step(paramA);
        if (bypass.cur != bypass.tgt) {
            if (bypass.cur < bypass.tgt) bypass.cur = std::min(bypass.tgt, bypass.cur + bypassStep);
            else bypass.cur = std::max(bypass.tgt, bypass.cur - bypassStep);
        }

        // ---- trim + stereo-linked compressor ------------------------------------------------------
        float t[2];
        float det = 0.0f;
        for (std::uint32_t c = 0; c < outCh; ++c) { t[c] = x[c] * trimLin; det = std::max(det, std::fabs(t[c])); }
        const float pk = detMax.push(det);
        const float grTarget = pk > 1.0e-9f ? gainReductionDb(linToDb(pk), thr.cur, ratio.cur, knee.cur) : 0.0f;
        const float a = grTarget > grState ? attA : relA;
        grState = grTarget + (grState - grTarget) * a;
        if (grState < 1.0e-6f && grTarget == 0.0f) grState = 0.0f;
        blockCompGrMax = std::max(blockCompGrMax, grState);
        const float compGain = std::exp(-grState * kLn10Over20) * makeupLin;
        float cmp[2];
        float cpk = 0.0f;
        for (std::uint32_t c = 0; c < outCh; ++c) { cmp[c] = t[c] * compGain; cpk = std::max(cpk, std::fabs(cmp[c])); }

        // ---- lookahead limiter ------------------------------------------------------------------------
        const float gl = lim.step(cmp, outCh, ceilLin, limRelA);
        if (gl < 0.9999f) ++blockLimSamples;
        blockLimMinGain = std::min(blockLimMinGain, gl);

        // ---- delayed wet / dry rings, bypass crossfade ---------------------------------------------
        const std::size_t base = static_cast<std::size_t>(ringPos) * outCh;
        const float b = bypass.cur;
        for (std::uint32_t c = 0; c < outCh; ++c) {
            const float wetOld = wetRing[base + c], dryOld = dryRing[base + c];
            wetRing[base + c] = cmp[c];
            dryRing[base + c] = x[c];
            float y = wetOld * gl;
            if (b > 0.0f) y = y * (1.0f - b) + dryOld * b;
            out[static_cast<std::size_t>(i) * outCh + c] = y;
            const float ay = std::fabs(y);
            if (ay > hold.outPeak) hold.outPeak = ay;
            if (ay >= 1.0f) ++hold.overSamples;
            if (wantTp) tp[c].push(y);
        }
        if (++ringPos == d) ringPos = 0;
    }

    // ---- publish block results into holds -----------------------------------------------------------
    if (wantTp) for (std::uint32_t c = 0; c < outCh; ++c) { hold.outTp = std::max(hold.outTp, tp[c].peak()); tp[c].resetPeak(); }
    hold.compGrMax = std::max(hold.compGrMax, blockCompGrMax);
    hold.limGrMaxLin = std::min(hold.limGrMaxLin, blockLimMinGain);
    hold.limSamples += blockLimSamples;
    compGrCur = grState;
    limGrCur = -linToDb(blockLimMinGain);
    if (limGrCur < 0.0f) limGrCur = 0.0f;
    limiterActiveLast = blockLimSamples > 0;
    frames += nFrames;
}

// ---- Impl -------------------------------------------------------------------------------------------
struct DynamicsProcessor::Impl {
    struct ParamStore {
        std::atomic<float> trim{0}, thr{-12}, ratio{3}, att{10}, rel{150}, knee{6}, makeup{0}, ceil{-1}, lrel{100};
        std::atomic<bool> bypass{false};
    } ps;
    struct Tele {
        std::atomic<float> inPeak{0}, outPeak{0}, outTp{0}, compCur{0}, compMax{0}, limCur{0}, limMaxLin{1};
        std::atomic<std::uint32_t> clipRunMax{0}, delay{0}, outCh{0}, inCh{0}, rate{0};
        std::atomic<std::uint64_t> nfs{0}, over{0}, lim{0}, sanitized{0}, frames{0};
        std::atomic<bool> limActive{false}, ready{false}, bypassed{false}, tpOn{false}, tpLimOn{false};
    } tele;
    std::atomic<Engine*> pending{nullptr};
    std::atomic<Engine*> retired{nullptr};
    std::atomic<bool> resetReq{false};
    Engine* active = nullptr;

    DynamicsParams loadParams() const {
        DynamicsParams p;
        p.trimDb = ps.trim.load(std::memory_order_relaxed);
        p.thresholdDb = ps.thr.load(std::memory_order_relaxed);
        p.ratio = ps.ratio.load(std::memory_order_relaxed);
        p.attackMs = ps.att.load(std::memory_order_relaxed);
        p.releaseMs = ps.rel.load(std::memory_order_relaxed);
        p.kneeDb = ps.knee.load(std::memory_order_relaxed);
        p.makeupDb = ps.makeup.load(std::memory_order_relaxed);
        p.ceilingDb = ps.ceil.load(std::memory_order_relaxed);
        p.limiterReleaseMs = ps.lrel.load(std::memory_order_relaxed);
        p.bypass = ps.bypass.load(std::memory_order_relaxed);
        return p;
    }
    void publish() {
        const Hold& h = active->hold;
        tele.inPeak.store(h.inPeak, std::memory_order_relaxed);
        tele.outPeak.store(h.outPeak, std::memory_order_relaxed);
        tele.outTp.store(h.outTp, std::memory_order_relaxed);
        tele.compCur.store(active->compGrCur, std::memory_order_relaxed);
        tele.compMax.store(h.compGrMax, std::memory_order_relaxed);
        tele.limCur.store(active->limGrCur, std::memory_order_relaxed);
        tele.limMaxLin.store(h.limGrMaxLin, std::memory_order_relaxed);
        tele.clipRunMax.store(h.clipRunMax, std::memory_order_relaxed);
        tele.nfs.store(h.nfsSamples, std::memory_order_relaxed);
        tele.over.store(h.overSamples, std::memory_order_relaxed);
        tele.lim.store(h.limSamples, std::memory_order_relaxed);
        tele.sanitized.store(active->sanitized, std::memory_order_relaxed);
        tele.frames.store(active->frames, std::memory_order_relaxed);
        tele.limActive.store(active->limiterActiveLast, std::memory_order_relaxed);
        tele.bypassed.store(active->bypass.tgt > 0.5f, std::memory_order_relaxed);
    }
};

std::string ParamReport::describe() const {
    static const struct { std::uint32_t bit; const char* name; } kNames[] = {
        {kPTrim, "trim"}, {kPThreshold, "threshold"}, {kPRatio, "ratio"}, {kPAttack, "attack"},
        {kPRelease, "release"}, {kPKnee, "knee"}, {kPMakeup, "makeup"}, {kPCeiling, "ceiling"},
        {kPLimiterRelease, "limiterRelease"}};
    std::string s;
    for (const auto& n : kNames)
        if (clampedMask & n.bit) { if (!s.empty()) s += ','; s += n.name; }
    return s;
}

DynamicsParams clampParams(const DynamicsParams& in, ParamReport* report) {
    DynamicsParams p = in;
    std::uint32_t m = 0;
    clampOne(p.trimDb, kTrim, m); clampOne(p.thresholdDb, kThr, m); clampOne(p.ratio, kRatio, m);
    clampOne(p.attackMs, kAtt, m); clampOne(p.releaseMs, kRel, m); clampOne(p.kneeDb, kKnee, m);
    clampOne(p.makeupDb, kMake, m); clampOne(p.ceilingDb, kCeil, m); clampOne(p.limiterReleaseMs, kLRel, m);
    if (report) report->clampedMask = m;
    return p;
}

DynamicsProcessor::DynamicsProcessor() : impl_(new Impl) {}

DynamicsProcessor::~DynamicsProcessor() {
    delete impl_->pending.exchange(nullptr);
    delete impl_->retired.exchange(nullptr);
    delete impl_->active;
    delete impl_;
}

Result<PrepareInfo> DynamicsProcessor::prepare(const DynamicsConfig& cfg) {
    if (cfg.sampleRateHz < 8000 || cfg.sampleRateHz > 384000)
        return makeError(ErrorCategory::InvalidArgument, "sample rate out of range 8000..384000");
    if (cfg.inChannels < 1 || cfg.inChannels > 2)
        return makeError(ErrorCategory::UnsupportedCapability, "dynamics supports 1 or 2 input channels");
    if (!std::isfinite(cfg.lookaheadMs) || cfg.lookaheadMs < 1.0f || cfg.lookaheadMs > 20.0f)
        return makeError(ErrorCategory::InvalidArgument, "lookahead out of range 1..20 ms");
    Engine* e = new Engine(cfg, impl_->loadParams());
    const PrepareInfo info = e->info;
    delete impl_->retired.exchange(nullptr);          // free the engine the audio thread retired earlier
    delete impl_->pending.exchange(e);                // replace a staged engine that was never adopted
    return info;
}

ParamReport DynamicsProcessor::setParams(const DynamicsParams& in) {
    ParamReport r;
    const DynamicsParams p = clampParams(in, &r);
    auto& s = impl_->ps;
    s.trim.store(p.trimDb, std::memory_order_relaxed); s.thr.store(p.thresholdDb, std::memory_order_relaxed);
    s.ratio.store(p.ratio, std::memory_order_relaxed); s.att.store(p.attackMs, std::memory_order_relaxed);
    s.rel.store(p.releaseMs, std::memory_order_relaxed); s.knee.store(p.kneeDb, std::memory_order_relaxed);
    s.makeup.store(p.makeupDb, std::memory_order_relaxed); s.ceil.store(p.ceilingDb, std::memory_order_relaxed);
    s.lrel.store(p.limiterReleaseMs, std::memory_order_relaxed); s.bypass.store(p.bypass, std::memory_order_relaxed);
    return r;
}

DynamicsParams DynamicsProcessor::params() const { return impl_->loadParams(); }

bool DynamicsProcessor::process(const float* in, float* out, std::uint32_t frames) {
    Impl& I = *impl_;
    if (I.pending.load(std::memory_order_acquire) && I.retired.load(std::memory_order_acquire) == nullptr) {
        if (Engine* e = I.pending.exchange(nullptr, std::memory_order_acq_rel)) {
            I.retired.store(I.active, std::memory_order_release);
            I.active = e;
            I.tele.delay.store(e->L, std::memory_order_relaxed);
            I.tele.inCh.store(e->inCh, std::memory_order_relaxed);
            I.tele.outCh.store(e->outCh, std::memory_order_relaxed);
            I.tele.rate.store(e->cfg.sampleRateHz, std::memory_order_relaxed);
            I.tele.tpOn.store(e->cfg.truePeakMeter, std::memory_order_relaxed);
            I.tele.tpLimOn.store(e->cfg.truePeakLimiter, std::memory_order_relaxed);
            I.tele.ready.store(true, std::memory_order_release);
        }
    }
    if (!I.active) return false;
    if (I.resetReq.exchange(false, std::memory_order_acq_rel)) I.active->resetHolds();
    I.active->process(in, out, frames, I.loadParams());
    I.publish();
    return true;
}

void DynamicsProcessor::resetHolds() { impl_->resetReq.store(true, std::memory_order_release); }

DynamicsTelemetry DynamicsProcessor::telemetry() const {
    const auto& t = impl_->tele;
    DynamicsTelemetry r;
    r.ready = t.ready.load(std::memory_order_acquire);
    r.bypassed = t.bypassed.load(std::memory_order_relaxed);
    r.inputPeakDb = linToDb(t.inPeak.load(std::memory_order_relaxed));
    r.inputClipRunMax = t.clipRunMax.load(std::memory_order_relaxed);
    r.inputNearFullScaleSamples = t.nfs.load(std::memory_order_relaxed);
    r.inputNearFullScale = r.inputNearFullScaleSamples > 0;
    r.inputPossibleClipping = r.inputClipRunMax >= kPossibleClipRun;
    r.outputPeakDb = linToDb(t.outPeak.load(std::memory_order_relaxed));
    r.outputTruePeakDb = t.tpOn.load(std::memory_order_relaxed) ? linToDb(t.outTp.load(std::memory_order_relaxed)) : kSilenceDb;
    r.outputOverFullScaleSamples = t.over.load(std::memory_order_relaxed);
    r.compressorGrDb = t.compCur.load(std::memory_order_relaxed);
    r.compressorGrMaxDb = t.compMax.load(std::memory_order_relaxed);
    r.limiterGrDb = t.limCur.load(std::memory_order_relaxed);
    const float lm = t.limMaxLin.load(std::memory_order_relaxed);
    r.limiterGrMaxDb = lm < 1.0f ? -linToDb(lm) : 0.0f;
    r.limiterActive = t.limActive.load(std::memory_order_relaxed);
    r.limiterActiveSamples = t.lim.load(std::memory_order_relaxed);
    r.sanitizedSamples = t.sanitized.load(std::memory_order_relaxed);
    r.processedFrames = t.frames.load(std::memory_order_relaxed);
    r.processingDelaySamples = t.delay.load(std::memory_order_relaxed);
    return r;
}

PrepareInfo DynamicsProcessor::activeInfo() const {
    const auto& t = impl_->tele;
    return {t.rate.load(std::memory_order_relaxed), t.inCh.load(std::memory_order_relaxed),
            t.outCh.load(std::memory_order_relaxed), t.delay.load(std::memory_order_relaxed)};
}

std::string DynamicsProcessor::metadataJson() const {
    const DynamicsParams p = params();
    const DynamicsTelemetry t = telemetry();
    const PrepareInfo i = activeInfo();
    const bool tpl = impl_->tele.tpLimOn.load(std::memory_order_relaxed);
    char buf[1536];
    std::snprintf(buf, sizeof buf,
        "{\"dynamics\":{\"enabled\":%s,\"bypassed\":%s,"
        "\"settings\":{\"trimDb\":%.2f,\"thresholdDb\":%.2f,\"ratio\":%.2f,\"attackMs\":%.2f,\"releaseMs\":%.1f,"
        "\"kneeDb\":%.2f,\"makeupDb\":%.2f,\"ceilingDb\":%.2f,\"ceilingUnit\":\"%s\","
        "\"limiterReleaseMs\":%.1f,\"limiterType\":\"%s\",\"trueTpLimiter\":%s},"
        "\"processingDelaySamples\":%u,\"sampleRateHz\":%u,"
        "\"rawInput\":{\"peakDb\":%.2f,\"nearFullScale\":%s,\"possibleClipping\":%s,\"nearFullScaleSamples\":%llu,\"maxRun\":%u},"
        "\"limiterControlledOutput\":{\"peakDb\":%.2f,\"truePeakEstimateDb\":%.2f,\"limiterActive\":%s,"
        "\"limiterActiveSamples\":%llu,\"limiterMaxGrDb\":%.2f,\"compressorMaxGrDb\":%.2f}}}",
        t.ready ? "true" : "false", p.bypass ? "true" : "false",
        static_cast<double>(p.trimDb), static_cast<double>(p.thresholdDb), static_cast<double>(p.ratio),
        static_cast<double>(p.attackMs), static_cast<double>(p.releaseMs), static_cast<double>(p.kneeDb),
        static_cast<double>(p.makeupDb), static_cast<double>(p.ceilingDb),
        tpl ? "dBTP" : "dBFS sample peak", static_cast<double>(p.limiterReleaseMs),
        tpl ? "true-peak lookahead (8x, synthetic-validated)" : "sample-peak lookahead", tpl ? "true" : "false",
        i.processingDelaySamples, i.sampleRateHz,
        static_cast<double>(t.inputPeakDb), t.inputNearFullScale ? "true" : "false",
        t.inputPossibleClipping ? "true" : "false", static_cast<unsigned long long>(t.inputNearFullScaleSamples),
        t.inputClipRunMax,
        static_cast<double>(t.outputPeakDb), static_cast<double>(t.outputTruePeakDb),
        t.limiterActiveSamples > 0 ? "true" : "false", static_cast<unsigned long long>(t.limiterActiveSamples),
        static_cast<double>(t.limiterGrMaxDb), static_cast<double>(t.compressorGrMaxDb));
    return buf;
}

}  // namespace ss::dynamics
