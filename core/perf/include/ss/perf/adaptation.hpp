// StageSignal perf (MOD-14): adaptive quality controller (REQ-P03, REQ-P04, section 7).
// Pure state machine driven by PressureSample ticks carrying their own monotonic time, so tests use fake time.
// It never touches the renderer: it returns an event and the app applies `state()` at a safe boundary.
#pragma once
#include <deque>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/perf/limits.hpp"

namespace ss::perf {

struct AdaptationConfig {
    // Triggers (any one starts shedding).
    double cpuTriggerPct = AdaptDefaults::kCpuTriggerPct;
    Ns cpuTriggerNs = AdaptDefaults::kCpuTriggerNs;
    double missTriggerFraction = AdaptDefaults::kMissTriggerFraction;   // strictly greater than
    Ns missWindowNs = AdaptDefaults::kMissWindowNs;
    double queueTrigger = AdaptDefaults::kQueueTrigger;
    Ns queueTriggerNs = AdaptDefaults::kQueueTriggerNs;
    Ns shedIntervalNs = AdaptDefaults::kShedIntervalNs;                  // at most one step per interval
    // Restore (all three calm conditions must hold for calmNs).
    double cpuCalmPct = AdaptDefaults::kCpuCalmPct;
    double missCalmFraction = AdaptDefaults::kMissCalmFraction;         // strictly less than
    double queueCalm = AdaptDefaults::kQueueCalm;
    Ns calmNs = AdaptDefaults::kCalmNs;
    Ns restoreIntervalNs = AdaptDefaults::kRestoreIntervalNs;
    // Levels applied by the steps.
    int initialSceneFps = 30;          // the scene-render rate before any shedding (BASE profile uses 25)
    double normalPreviewFps = 15.0;
    double shedPreviewFps = 5.0;
    double effectsScaleShed = 0.5;     // trails/particle budget multiplier after the "reduce trails/particles" step
    double renderScaleShed = 0.75;     // internal render scale after the "lower internal render scale" step

    // Validated ranges; returns InvalidArgument with a sanitized message naming the field.
    Status validate() const;
};

enum class AdaptStepKind : std::uint8_t { PreviewFps, ReduceEffects, LowerRenderScale, ReduceSourceRequest, LowerSceneFps };

struct AdaptationState {
    int step = 0;                      // 0 = full quality
    int maxStep = 0;
    double previewFps = 15.0;
    double effectsScale = 1.0;         // 1.0 = full trails/particles
    double renderScale = 1.0;
    bool sourceRequestReduced = false; // ask camera/decoder for a cheaper mode where renegotiation is supported
    int sceneFps = 30;
    bool sceneFpsLowered = false;      // true once any LowerSceneFps step is applied
};

struct PressureSample {
    Ns now = 0;                        // monotonic
    bool cpuAvailable = false;
    double cpuPct = 0;                 // process/system CPU, whichever the caller prefers, normalised 0..100
    std::uint64_t deadlinesDue = 0, deadlinesMissed = 0;   // deltas since the previous sample
    bool queueAvailable = false;
    double encoderQueue = 0;           // 0..1 occupancy of the render->encoder queue
};

struct AdaptationEvent {
    enum class Kind : std::uint8_t { None, Shed, Restore, OverloadWarning } kind = Kind::None;
    int fromStep = 0, toStep = 0;
    AdaptStepKind stepKind = AdaptStepKind::PreviewFps;   // meaningful for Shed/Restore
    std::string detail;                                   // sanitized, journal-ready ("adapt" kind)
};

class AdaptationController {
public:
    explicit AdaptationController(const AdaptationConfig& cfg = {});
    // Replaces the config if valid; otherwise leaves the controller unchanged. Does not reset the step.
    Status setConfig(const AdaptationConfig& cfg);
    const AdaptationConfig& config() const { return cfg_; }

    void setLocked(bool locked) { locked_ = locked; }
    bool locked() const { return locked_; }

    AdaptationEvent update(const PressureSample& s);

    int step() const { return step_; }
    int maxStep() const { return static_cast<int>(ladder_.size()); }
    AdaptationState state() const;
    bool overloadWarningActive() const { return warning_; }
    // Added work (new camera, second output, encoder...) is refused while locked quality is overloaded or
    // while shedding is still in effect. Never changes any quality setting.
    bool admitAddedWork(std::string* reason = nullptr) const;
    void reset();

    static std::string describeStep(AdaptStepKind k, int sceneFpsAfter);

private:
    struct Rec { Ns t; std::uint64_t due, missed; };
    void buildLadder();
    bool missFraction(Ns window, double* out) const;   // false until `window` of history exists
    AdaptationConfig cfg_;
    std::vector<AdaptStepKind> ladder_;
    std::vector<int> sceneFpsLevels_;                  // fps after each LowerSceneFps step, in ladder order
    int step_ = 0;
    bool locked_ = false, warning_ = false, triggeredPrev_ = false;
    Ns lastChange_;
    Ns firstSample_ = -1;
    Ns cpuHighSince_ = -1, queueHighSince_ = -1, cpuLowSince_ = -1, queueLowSince_ = -1;
    std::deque<Rec> hist_;
};

}  // namespace ss::perf
