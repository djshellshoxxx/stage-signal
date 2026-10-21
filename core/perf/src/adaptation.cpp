#include "ss/perf/adaptation.hpp"
#include <algorithm>
#include <limits>

namespace ss::perf {

namespace {
constexpr Ns kNone = std::numeric_limits<Ns>::min();
const int kSceneFpsLadder[] = {30, 25, 20, 15};

Status bad(const char* field) {
    return Status(makeError(ErrorCategory::InvalidArgument, std::string("adaptation config out of range: ") + field,
                            Recoverability::OperatorAction));
}
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }
bool inRangeNs(Ns v, Ns lo, Ns hi) { return v >= lo && v <= hi; }
}  // namespace

Status AdaptationConfig::validate() const {
    if (!inRange(cpuTriggerPct, 50.0, 100.0)) return bad("cpuTriggerPct");
    if (!inRange(cpuCalmPct, 10.0, 95.0)) return bad("cpuCalmPct");
    if (cpuCalmPct >= cpuTriggerPct) return bad("cpuCalmPct must be below cpuTriggerPct");
    if (!inRangeNs(cpuTriggerNs, kNsPerSec, 60 * kNsPerSec)) return bad("cpuTriggerNs");
    if (!inRange(missTriggerFraction, 0.005, 0.5)) return bad("missTriggerFraction");
    if (!inRange(missCalmFraction, 0.0, 0.2) || missCalmFraction >= missTriggerFraction) return bad("missCalmFraction");
    if (!inRangeNs(missWindowNs, kNsPerSec, 60 * kNsPerSec)) return bad("missWindowNs");
    if (!inRange(queueTrigger, 0.3, 1.0)) return bad("queueTrigger");
    if (!inRange(queueCalm, 0.0, 0.9) || queueCalm >= queueTrigger) return bad("queueCalm");
    if (!inRangeNs(queueTriggerNs, kNsPerSec, 60 * kNsPerSec)) return bad("queueTriggerNs");
    if (!inRangeNs(shedIntervalNs, kNsPerSec, 120 * kNsPerSec)) return bad("shedIntervalNs");
    if (!inRangeNs(calmNs, 5 * kNsPerSec, 600 * kNsPerSec)) return bad("calmNs");
    if (!inRangeNs(restoreIntervalNs, 5 * kNsPerSec, 600 * kNsPerSec)) return bad("restoreIntervalNs");
    if (initialSceneFps < 15 || initialSceneFps > 60) return bad("initialSceneFps");
    if (!inRange(normalPreviewFps, 1.0, 60.0)) return bad("normalPreviewFps");
    if (!inRange(shedPreviewFps, 1.0, 30.0) || shedPreviewFps > normalPreviewFps) return bad("shedPreviewFps");
    if (!inRange(effectsScaleShed, 0.0, 1.0)) return bad("effectsScaleShed");
    if (!inRange(renderScaleShed, 0.25, 1.0)) return bad("renderScaleShed");
    return okStatus();
}

AdaptationController::AdaptationController(const AdaptationConfig& cfg) : cfg_(cfg.validate().ok() ? cfg : AdaptationConfig{}) {
    buildLadder();
    reset();
}

void AdaptationController::buildLadder() {
    ladder_ = {AdaptStepKind::PreviewFps, AdaptStepKind::ReduceEffects, AdaptStepKind::LowerRenderScale,
               AdaptStepKind::ReduceSourceRequest};
    sceneFpsLevels_.clear();
    for (int f : kSceneFpsLadder) {
        if (f < cfg_.initialSceneFps) { ladder_.push_back(AdaptStepKind::LowerSceneFps); sceneFpsLevels_.push_back(f); }
    }
}

Status AdaptationController::setConfig(const AdaptationConfig& cfg) {
    Status s = cfg.validate();
    if (!s.ok()) return s;
    cfg_ = cfg;
    buildLadder();
    if (step_ > static_cast<int>(ladder_.size())) step_ = static_cast<int>(ladder_.size());
    return okStatus();
}

void AdaptationController::reset() {
    step_ = 0; warning_ = false; triggeredPrev_ = false;
    lastChange_ = kNone;
    firstSample_ = kNone;
    cpuHighSince_ = queueHighSince_ = cpuLowSince_ = queueLowSince_ = kNone;
    hist_.clear();
}

AdaptationState AdaptationController::state() const {
    AdaptationState s;
    s.step = step_;
    s.maxStep = static_cast<int>(ladder_.size());
    s.previewFps = cfg_.normalPreviewFps;
    s.sceneFps = cfg_.initialSceneFps;
    int sceneIdx = 0;
    for (int i = 0; i < step_ && i < static_cast<int>(ladder_.size()); ++i) {
        switch (ladder_[static_cast<std::size_t>(i)]) {
            case AdaptStepKind::PreviewFps: s.previewFps = cfg_.shedPreviewFps; break;
            case AdaptStepKind::ReduceEffects: s.effectsScale = cfg_.effectsScaleShed; break;
            case AdaptStepKind::LowerRenderScale: s.renderScale = cfg_.renderScaleShed; break;
            case AdaptStepKind::ReduceSourceRequest: s.sourceRequestReduced = true; break;
            case AdaptStepKind::LowerSceneFps:
                s.sceneFps = sceneFpsLevels_[static_cast<std::size_t>(sceneIdx++)];
                s.sceneFpsLowered = true;
                break;
        }
    }
    return s;
}

bool AdaptationController::missFraction(Ns window, double* out) const {
    if (hist_.empty() || firstSample_ == kNone) return false;
    const Ns now = hist_.back().t;
    if (now - firstSample_ < window) return false;
    std::uint64_t due = 0, missed = 0;
    for (const Rec& r : hist_) {
        if (r.t > now - window) { due += r.due; missed += r.missed; }
    }
    *out = due ? static_cast<double>(missed) / static_cast<double>(due) : 0.0;
    return true;
}

std::string AdaptationController::describeStep(AdaptStepKind k, int sceneFpsAfter) {
    switch (k) {
        case AdaptStepKind::PreviewFps: return "operator preview reduced to 5 FPS";
        case AdaptStepKind::ReduceEffects: return "trails and particles reduced";
        case AdaptStepKind::LowerRenderScale: return "internal render scale lowered";
        case AdaptStepKind::ReduceSourceRequest: return "source decode/camera request reduced";
        case AdaptStepKind::LowerSceneFps: return "scene render rate lowered to " + std::to_string(sceneFpsAfter) + " FPS";
    }
    return "";
}

bool AdaptationController::admitAddedWork(std::string* reason) const {
    if (locked_ && (warning_ || triggeredPrev_)) {
        if (reason) *reason = "Quality is locked and the system is overloaded; added work refused";
        return false;
    }
    if (!locked_ && step_ > 0) {
        if (reason) *reason = "Adaptive quality is still shedding load; added work refused";
        return false;
    }
    return true;
}

AdaptationEvent AdaptationController::update(const PressureSample& s) {
    AdaptationEvent ev;
    const Ns now = s.now;
    if (firstSample_ == kNone) firstSample_ = now;
    hist_.push_back(Rec{now, s.deadlinesDue, s.deadlinesMissed});
    const Ns keep = std::max(cfg_.missWindowNs, cfg_.calmNs) + kNsPerSec;
    while (hist_.size() > 1 && hist_.front().t < now - keep) hist_.pop_front();

    // Sustained-condition timers.
    if (s.cpuAvailable && s.cpuPct >= cfg_.cpuTriggerPct) { if (cpuHighSince_ == kNone) cpuHighSince_ = now; }
    else cpuHighSince_ = kNone;
    if (s.queueAvailable && s.encoderQueue >= cfg_.queueTrigger) { if (queueHighSince_ == kNone) queueHighSince_ = now; }
    else queueHighSince_ = kNone;
    if (s.cpuAvailable && s.cpuPct < cfg_.cpuCalmPct) { if (cpuLowSince_ == kNone) cpuLowSince_ = now; }
    else cpuLowSince_ = kNone;                       // unknown CPU is never "calm"
    if (!s.queueAvailable || s.encoderQueue < cfg_.queueCalm) { if (queueLowSince_ == kNone) queueLowSince_ = now; }
    else queueLowSince_ = kNone;                     // no encoder queue => nothing to hold restore back

    const bool cpuTrig = cpuHighSince_ != kNone && now - cpuHighSince_ >= cfg_.cpuTriggerNs;
    const bool queueTrig = queueHighSince_ != kNone && now - queueHighSince_ >= cfg_.queueTriggerNs;
    double mf = 0;
    const bool missTrig = missFraction(cfg_.missWindowNs, &mf) && mf > cfg_.missTriggerFraction;
    const bool triggered = cpuTrig || missTrig || queueTrig;

    double calmMf = 0;
    const bool calm = cpuLowSince_ != kNone && now - cpuLowSince_ >= cfg_.calmNs &&
                      queueLowSince_ != kNone && now - queueLowSince_ >= cfg_.calmNs &&
                      missFraction(cfg_.calmNs, &calmMf) && calmMf < cfg_.missCalmFraction;

    const int maxStep = static_cast<int>(ladder_.size());
    if (triggered) {
        if (locked_) {
            if (!triggeredPrev_ || !warning_) {
                warning_ = true;
                ev.kind = AdaptationEvent::Kind::OverloadWarning;
                ev.fromStep = ev.toStep = step_;
                ev.detail = "overload while quality is locked; profile unchanged";
            }
        } else if (step_ < maxStep) {
            if (lastChange_ == kNone || now - lastChange_ >= cfg_.shedIntervalNs) {
                ev.kind = AdaptationEvent::Kind::Shed;
                ev.fromStep = step_;
                ev.toStep = ++step_;
                ev.stepKind = ladder_[static_cast<std::size_t>(step_ - 1)];
                lastChange_ = now;
                int sceneAfter = cfg_.initialSceneFps;
                if (ev.stepKind == AdaptStepKind::LowerSceneFps) sceneAfter = state().sceneFps;
                ev.detail = describeStep(ev.stepKind, sceneAfter);
            }
        } else if (!warning_) {
            warning_ = true;
            ev.kind = AdaptationEvent::Kind::OverloadWarning;
            ev.fromStep = ev.toStep = step_;
            ev.detail = "overload persists at maximum shedding";
        }
    } else if (calm) {
        warning_ = false;
        if (!locked_ && step_ > 0 && (lastChange_ == kNone || now - lastChange_ >= cfg_.restoreIntervalNs)) {
            ev.kind = AdaptationEvent::Kind::Restore;
            ev.fromStep = step_;
            ev.stepKind = ladder_[static_cast<std::size_t>(step_ - 1)];
            --step_;
            ev.toStep = step_;
            lastChange_ = now;
            ev.detail = "restored one step (now step " + std::to_string(step_) + " of " + std::to_string(maxStep) + ")";
        }
    }
    triggeredPrev_ = triggered;
    return ev;
}

}  // namespace ss::perf
