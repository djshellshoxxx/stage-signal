// MOD-14 adaptive quality (spec §7, REQ-P03/P04). Pure state machine driven by PressureSample (no threads, no I/O).
// Triggers: process/system CPU >= 85% for 5 s, OR >5% missed deadlines over 5 s, OR encoder queue >= 75% for 2 s.
// Ladder (one step per >=5 s): preview 5 FPS -> trails/particles -> render scale -> source decode/camera request
// -> scene FPS 30->25->20->15. Restore one step per >=30 s after CPU <65%, misses <1%, queues <25% held for 30 s.
// Locked quality: never changes level (no shed, no restore); raises warnings and rejects added work instead.
// Steps unavailable on this build (no trails, no renegotiation, base FPS already low) are skipped, not faked.
#pragma once
#include "core/types.h"
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace ss {

enum class AdaptStep : uint8_t { None, PreviewFps5, TrailsParticles, RenderScale, SourceRequest, SceneFps25, SceneFps20, SceneFps15 };
const char* adaptStepName(AdaptStep s);

struct AdaptationConfig {                       // tunable; validate() reports out-of-range, sanitized() clamps
  double cpuTriggerPct = 85; Ns cpuTriggerNs = 5 * kNsPerSec;
  double missTrigger = 0.05; Ns missWindowNs = 5 * kNsPerSec;
  double queueTrigger = 0.75; Ns queueTriggerNs = 2 * kNsPerSec;
  Ns stepIntervalNs = 5 * kNsPerSec;
  double restoreCpuPct = 65, restoreMiss = 0.01, restoreQueue = 0.25; Ns restoreHoldNs = 30 * kNsPerSec; Ns restoreIntervalNs = 30 * kNsPerSec;
  double baseSceneFps = 30; double previewFpsNormal = 30; int previewFpsReduced = 5; double renderScaleStep = 0.75;
  bool sourceRequestSupported = true;           // renegotiation of decode/camera request available
  bool trailsParticlesPresent = true;
  Status validate() const;
  AdaptationConfig sanitized() const;
};

struct QualityState {                           // what the renderer/media layers should apply now
  int level = 0; AdaptStep lastStep = AdaptStep::None;
  double previewFps = 30; bool trailsParticlesReduced = false; double renderScale = 1.0; bool sourceRequestReduced = false; double sceneFps = 30;
};
struct PressureSample {
  Ns now = 0;
  bool cpuAvailable = false; double processCpuPct = 0, systemCpuPct = 0;      // normalized 0..100
  uint64_t deadlinesEvaluated = 0, deadlinesMissed = 0;                       // CUMULATIVE since start
  double encoderQueueFraction = 0;                                            // 0..1
  double maxQueueFraction = 0;                                                // 0..1 over all bounded queues
};
enum class AdaptEventKind : uint8_t { StepDown, StepUp, LockedOverloadWarning, LadderExhausted, WorkRejected };
struct AdaptationEvent { Ns t = 0; AdaptEventKind kind = AdaptEventKind::StepDown; int level = 0; AdaptStep step = AdaptStep::None; std::string reason; };
const char* adaptEventKindName(AdaptEventKind k);

class AdaptationController {
 public:
  explicit AdaptationController(AdaptationConfig cfg = AdaptationConfig());
  void setLocked(bool locked) { locked_ = locked; }
  bool locked() const { return locked_; }
  QualityState update(const PressureSample& s);         // monotonic `now`; older samples ignored
  QualityState state() const { return stateFor(level_); }
  int level() const { return level_; }
  int maxLevel() const { return (int)ladder_.size(); }
  const std::vector<AdaptStep>& ladder() const { return ladder_; }
  bool overloadActive() const { return overload_; }
  // Ask permission for added work (new output, recording, clip...). False + reason when overloaded (REQ-P04).
  bool tryAddWork(const std::string& what, std::string* reason = nullptr);
  uint64_t warnings() const { return warnings_; }
  uint64_t rejectedWork() const { return rejected_; }
  const std::deque<AdaptationEvent>& events() const { return events_; }       // last 64
  void setEventCallback(std::function<void(const AdaptationEvent&)> cb) { onEvent_ = std::move(cb); }
  void setQualityCallback(std::function<void(const QualityState&)> cb) { onQuality_ = std::move(cb); }
  const AdaptationConfig& config() const { return cfg_; }
 private:
  QualityState stateFor(int level) const;
  void emit(AdaptEventKind k, Ns t, const std::string& why);
  AdaptationConfig cfg_; std::vector<AdaptStep> ladder_; int level_ = 0; bool locked_ = false, overload_ = false;
  Ns lastNow_ = 0; bool haveNow_ = false;
  Ns cpuSince_ = -1, encSince_ = -1, calmSince_ = -1, lastStep_ = 0, lastWarn_ = 0, lastExhaust_ = 0; bool haveStep_ = false, haveWarn_ = false, haveExhaust_ = false;
  struct W { Ns t; uint64_t eval, miss; }; std::deque<W> win_; uint64_t prevEval_ = 0, prevMiss_ = 0; Ns firstSample_ = -1;
  uint64_t warnings_ = 0, rejected_ = 0; std::deque<AdaptationEvent> events_;
  std::function<void(const AdaptationEvent&)> onEvent_; std::function<void(const QualityState&)> onQuality_;
};

} // namespace ss
