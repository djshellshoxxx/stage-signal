#include "perf/adaptation.h"
#include <algorithm>

namespace ss {

const char* adaptStepName(AdaptStep s) {
  static const char* n[] = {"none", "preview_5fps", "trails_particles", "render_scale", "source_request", "scene_fps_25", "scene_fps_20", "scene_fps_15"};
  return (size_t)s < 8 ? n[(size_t)s] : "?";
}
const char* adaptEventKindName(AdaptEventKind k) {
  static const char* n[] = {"step_down", "step_up", "locked_overload_warning", "ladder_exhausted", "work_rejected"};
  return (size_t)k < 5 ? n[(size_t)k] : "?";
}

Status AdaptationConfig::validate() const {
  auto bad = [](const char* w) { return Status::Fail(Err::InvalidArgument, std::string("adaptation config out of range: ") + w); };
  if (cpuTriggerPct < 50 || cpuTriggerPct > 100) return bad("cpuTriggerPct");
  if (cpuTriggerNs < kNsPerSec || cpuTriggerNs > 60 * kNsPerSec) return bad("cpuTriggerNs");
  if (missTrigger < 0.01 || missTrigger > 0.5) return bad("missTrigger");
  if (missWindowNs < kNsPerSec || missWindowNs > 60 * kNsPerSec) return bad("missWindowNs");
  if (queueTrigger < 0.3 || queueTrigger > 1.0) return bad("queueTrigger");
  if (queueTriggerNs < kNsPerSec / 2 || queueTriggerNs > 30 * kNsPerSec) return bad("queueTriggerNs");
  if (stepIntervalNs < kNsPerSec || stepIntervalNs > 60 * kNsPerSec) return bad("stepIntervalNs");
  if (restoreIntervalNs < 5 * kNsPerSec || restoreIntervalNs > 600 * kNsPerSec) return bad("restoreIntervalNs");
  if (restoreHoldNs < 5 * kNsPerSec || restoreHoldNs > 600 * kNsPerSec) return bad("restoreHoldNs");
  if (restoreCpuPct < 20 || restoreCpuPct > cpuTriggerPct - 5) return bad("restoreCpuPct (needs >=5 points hysteresis below trigger)");
  if (restoreMiss < 0 || restoreMiss >= missTrigger) return bad("restoreMiss");
  if (restoreQueue < 0.05 || restoreQueue >= queueTrigger) return bad("restoreQueue");
  if (baseSceneFps < 15 || baseSceneFps > 60) return bad("baseSceneFps");
  if (previewFpsNormal < 5 || previewFpsNormal > 120) return bad("previewFpsNormal");
  if (previewFpsReduced < 1 || previewFpsReduced > previewFpsNormal) return bad("previewFpsReduced");
  if (renderScaleStep < 0.4 || renderScaleStep > 0.95) return bad("renderScaleStep");
  return Status::Ok();
}
AdaptationConfig AdaptationConfig::sanitized() const {
  AdaptationConfig c = *this;
  auto cl = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
  auto cn = [](Ns v, Ns lo, Ns hi) { return v < lo ? lo : (v > hi ? hi : v); };
  c.cpuTriggerPct = cl(c.cpuTriggerPct, 50, 100); c.cpuTriggerNs = cn(c.cpuTriggerNs, kNsPerSec, 60 * kNsPerSec);
  c.missTrigger = cl(c.missTrigger, 0.01, 0.5); c.missWindowNs = cn(c.missWindowNs, kNsPerSec, 60 * kNsPerSec);
  c.queueTrigger = cl(c.queueTrigger, 0.3, 1.0); c.queueTriggerNs = cn(c.queueTriggerNs, kNsPerSec / 2, 30 * kNsPerSec);
  c.stepIntervalNs = cn(c.stepIntervalNs, kNsPerSec, 60 * kNsPerSec);
  c.restoreIntervalNs = cn(c.restoreIntervalNs, 5 * kNsPerSec, 600 * kNsPerSec); c.restoreHoldNs = cn(c.restoreHoldNs, 5 * kNsPerSec, 600 * kNsPerSec);
  c.restoreCpuPct = cl(c.restoreCpuPct, 20, c.cpuTriggerPct - 5); c.restoreMiss = cl(c.restoreMiss, 0, c.missTrigger * 0.99);
  c.restoreQueue = cl(c.restoreQueue, 0.05, c.queueTrigger * 0.99);
  c.baseSceneFps = cl(c.baseSceneFps, 15, 60); c.previewFpsNormal = cl(c.previewFpsNormal, 5, 120);
  c.previewFpsReduced = (int)cl(c.previewFpsReduced, 1, c.previewFpsNormal); c.renderScaleStep = cl(c.renderScaleStep, 0.4, 0.95);
  return c;
}

AdaptationController::AdaptationController(AdaptationConfig cfg) : cfg_(cfg.sanitized()) {
  ladder_.push_back(AdaptStep::PreviewFps5);
  if (cfg_.trailsParticlesPresent) ladder_.push_back(AdaptStep::TrailsParticles);
  ladder_.push_back(AdaptStep::RenderScale);
  if (cfg_.sourceRequestSupported) ladder_.push_back(AdaptStep::SourceRequest);
  if (cfg_.baseSceneFps > 25) ladder_.push_back(AdaptStep::SceneFps25);
  if (cfg_.baseSceneFps > 20) ladder_.push_back(AdaptStep::SceneFps20);
  if (cfg_.baseSceneFps > 15) ladder_.push_back(AdaptStep::SceneFps15);
}

QualityState AdaptationController::stateFor(int level) const {
  QualityState q; q.level = level; q.previewFps = cfg_.previewFpsNormal; q.sceneFps = cfg_.baseSceneFps;
  for (int i = 0; i < level && i < (int)ladder_.size(); ++i) {
    q.lastStep = ladder_[i];
    switch (ladder_[i]) {
      case AdaptStep::PreviewFps5: q.previewFps = cfg_.previewFpsReduced; break;
      case AdaptStep::TrailsParticles: q.trailsParticlesReduced = true; break;
      case AdaptStep::RenderScale: q.renderScale = cfg_.renderScaleStep; break;
      case AdaptStep::SourceRequest: q.sourceRequestReduced = true; break;
      case AdaptStep::SceneFps25: q.sceneFps = std::min(q.sceneFps, 25.0); break;
      case AdaptStep::SceneFps20: q.sceneFps = std::min(q.sceneFps, 20.0); break;
      case AdaptStep::SceneFps15: q.sceneFps = std::min(q.sceneFps, 15.0); break;
      default: break;
    }
  }
  return q;
}

void AdaptationController::emit(AdaptEventKind k, Ns t, const std::string& why) {
  AdaptationEvent e; e.t = t; e.kind = k; e.level = level_; e.step = level_ > 0 ? ladder_[level_ - 1] : AdaptStep::None; e.reason = why;
  if (events_.size() >= 64) events_.pop_front();
  events_.push_back(e); if (onEvent_) onEvent_(e);
}

QualityState AdaptationController::update(const PressureSample& s) {
  if (haveNow_ && s.now < lastNow_) return state();
  haveNow_ = true; lastNow_ = s.now; const Ns now = s.now;
  if (firstSample_ < 0) firstSample_ = now;
  // --- observe
  double cpu = s.cpuAvailable ? std::max(s.processCpuPct, s.systemCpuPct) : 0;
  bool cpuHigh = s.cpuAvailable && cpu >= cfg_.cpuTriggerPct;
  if (cpuHigh) { if (cpuSince_ < 0) cpuSince_ = now; } else cpuSince_ = -1;
  bool encHigh = s.encoderQueueFraction >= cfg_.queueTrigger;
  if (encHigh) { if (encSince_ < 0) encSince_ = now; } else encSince_ = -1;
  uint64_t dE = s.deadlinesEvaluated >= prevEval_ ? s.deadlinesEvaluated - prevEval_ : 0, dM = s.deadlinesMissed >= prevMiss_ ? s.deadlinesMissed - prevMiss_ : 0;
  prevEval_ = s.deadlinesEvaluated; prevMiss_ = s.deadlinesMissed;
  win_.push_back({now, dE, dM});
  while (!win_.empty() && win_.front().t <= now - cfg_.missWindowNs) win_.pop_front();
  if (win_.size() > 4096) win_.pop_front();
  uint64_t wE = 0, wM = 0; for (auto& w : win_) { wE += w.eval; wM += w.miss; }
  double missFrac = wE ? (double)wM / (double)wE : 0;
  bool windowCovered = now - firstSample_ >= cfg_.missWindowNs;
  bool missHigh = windowCovered && wE > 0 && missFrac > cfg_.missTrigger;
  bool cpuTrig = cpuSince_ >= 0 && now - cpuSince_ >= cfg_.cpuTriggerNs;
  bool encTrig = encSince_ >= 0 && now - encSince_ >= cfg_.queueTriggerNs;
  bool triggered = cpuTrig || missHigh || encTrig;
  double maxQ = std::max(s.maxQueueFraction, s.encoderQueueFraction);
  bool calm = (!s.cpuAvailable || cpu < cfg_.restoreCpuPct) && missFrac < cfg_.restoreMiss && maxQ < cfg_.restoreQueue;
  std::string why = cpuTrig ? "cpu>=trigger sustained" : (missHigh ? "missed deadlines>trigger over window" : "encoder queue>=trigger sustained");
  // --- act
  if (triggered) {
    overload_ = true; calmSince_ = -1;
    if (locked_) {
      if (!haveWarn_ || now - lastWarn_ >= cfg_.stepIntervalNs) { haveWarn_ = true; lastWarn_ = now; ++warnings_; emit(AdaptEventKind::LockedOverloadWarning, now, "quality locked, overload: " + why); }
    } else if (!haveStep_ || now - lastStep_ >= cfg_.stepIntervalNs) {
      if (level_ < (int)ladder_.size()) {
        ++level_; haveStep_ = true; lastStep_ = now; emit(AdaptEventKind::StepDown, now, why);
        if (onQuality_) onQuality_(state());
      } else if (!haveExhaust_ || now - lastExhaust_ >= cfg_.stepIntervalNs) {
        haveExhaust_ = true; lastExhaust_ = now; ++warnings_; emit(AdaptEventKind::LadderExhausted, now, "overload persists at maximum shedding: " + why);
      }
    }
  } else if (calm) {
    overload_ = false;
    if (calmSince_ < 0) calmSince_ = now;
    if (!locked_ && level_ > 0 && now - calmSince_ >= cfg_.restoreHoldNs) {
      emit(AdaptEventKind::StepUp, now, "recovered"); // emitted with pre-restore level, then lowered
      --level_; calmSince_ = now; events_.back().level = level_;
      if (onQuality_) onQuality_(state());
    }
  } else {
    calmSince_ = -1;                 // hysteresis band: neither shed nor restore
  }
  return state();
}

bool AdaptationController::tryAddWork(const std::string& what, std::string* reason) {
  if (!overload_) return true;
  ++rejected_;
  std::string r = locked_ ? "Quality is locked and the system is overloaded: '" + what + "' was not added. Unlock quality or remove load."
                          : "System is under pressure (adaptation level " + std::to_string(level_) + "): '" + what + "' was not added until load recovers.";
  if (reason) *reason = r;
  emit(AdaptEventKind::WorkRejected, lastNow_, "rejected added work");
  return false;
}

} // namespace ss
