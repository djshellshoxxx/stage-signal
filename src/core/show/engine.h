// ShowEngine (MOD-04): the single writer of show state. Not internally synchronized: the action broker /
// engine thread calls submit() and update(); other threads receive ShowSnapshot copies.
// Deterministic: all time comes from the Ns passed in (FakeClock-friendly); PRNG and weights are integer-only.
#pragma once
#include "core/show/model.h"
#include <functional>
#include <string>
#include <vector>

namespace ss {
namespace show {

enum class ShowMode : uint8_t { Auto, Programmed, Hybrid };

struct OutputState {
  std::string logicalId;
  OutputRole role = OutputRole::Audience;
  bool selected = false;               // currently targeted by operator controls
  int sceneIndex = -1;                 // effective scene (-1 none)
  bool independentScene = false;       // true if this output was given its own scene (else follows program)
  float brightness = 1.0f;             // 0..1
  float speed = 1.0f;                  // 0.25..4
  float intensity = 1.0f;              // 0..1
  float hue = 0.0f;                    // 0..1
  std::string publishedText;           // audience text (changes only via Publish)
  std::string publishedTextId;
};

// Immutable view for renderer / lighting / UI. 'revision' counts discrete state changes (continuous values such as
// transitionProgress, cueElapsedSec and estimated tempo/key are NOT revisioned).
struct ShowSnapshot {
  uint64_t revision = 0;
  ShowMode mode = ShowMode::Auto;
  bool paused = false;                 // PauseAutomation latch
  bool manualOverride = false;         // operator took scene control; no auto scene/cue fires until Resume
  int cueIndex = -1;                   // active cue (-1 none)
  bool cueEnded = false;               // duration elapsed, holding at end of list
  bool cueWaitingForTempo = false;     // beat-length cue with no valid tempo (never assumes a BPM)
  double cueElapsedSec = 0;
  double cueRemainingSec = -1;         // -1 hold / unknown
  std::string cueMediaRef, cueTextRef, cueLightingRef;   // textRef is a suggestion; never auto-published
  bool cueMediaMissing = false;
  int sceneIndex = -1;                 // program scene
  int outgoingSceneIndex = -1;         // during a transition
  float transitionProgress = 1.0f;     // 1 = complete
  std::string transitionType;
  std::vector<OutputState> outputs;
  int target = -1;                     // selected target output (-1 = all visual outputs)
  int bank = 0;
  bool visualBlackoutLatch = false, lightingBlackoutLatch = false, emergencyBlackoutLatch = false;
  bool visualBlackedOut = false;       // effective: visual || emergency
  bool lightingBlackedOut = false;     // effective: lighting || emergency
  bool strobeDisabled = false;         // lighting + renderer must not produce flash/strobe
  bool sceneStrobeSuppressed = false;  // current scene is strobe-capable but strobing is disabled
  bool lightingArmRequested = false;   // desired arm state; always false after load/restart
  uint64_t seed = 0;
  Estimate tempo, key;                 // effective (manual/tap override merged); source field tells which
  bool tempoOverridden = false, keyOverridden = false;
  bool audioLost = false;
  bool idleActive = false;             // idle screen policy engaged
  std::string idleScreen;
  std::string draftText, previewText;  // text pipeline: draft -> preview -> publish
  bool programmedWaitBoundary = false; // next cue / scene change is waiting for beat/bar boundary
  size_t pendingActions = 0;
};

struct EngineOptions {
    bool startIdle = false;            // crash-recovery start: idle screen until operator acts
    int tapWindow = 8;                 // taps averaged (last N)
    int tapResetGapMs = 2500;          // gap that restarts a tap sequence
};

class ShowEngine {
public:
  using Options = EngineOptions;
  using JournalHook = std::function<void(const std::string& kind, const std::string& json)>;

  explicit ShowEngine(Options o = EngineOptions());
  // Validates the FULL document first; on failure the running show is untouched (REQ-S14).
  Status replaceDocument(const ShowDocument& doc);
  // Test/diagnostic: adopt without validation to exercise runtime missing-reference handling.
  void adoptUnvalidated(const ShowDocument& doc);
  const ShowDocument& document() const { return doc_; }
  // Document with the live overrides (manual BPM/key, strobe disable) folded in for saving (REQ-A06/A08).
  ShowDocument documentWithState() const;

  // IF-02. Validates, applies idempotent desired state, acknowledges with the resulting revision.
  ActionResult submit(const Action& a);
  ActionResult submit(const Action& a, Ns now);
  static bool isShowAction(ActionId id);

  // ~60 Hz tick. Features supply estimated tempo/key and audio liveness (FeatureSnapshot::sequence must advance).
  void update(Ns now, const FeatureSnapshot& f);
  void setAudioAvailable(bool available);          // explicit device notification (REQ-A02 path)

  ShowSnapshot snapshot() const;
  FeatureSnapshot effectiveFeatures() const;       // last features with tempo/key replaced by effective values

  // Text pipeline (REQ-S05): editing never touches audience outputs. Only publish*() / Action::PublishText do.
  Status setDraft(const std::string& utf8);
  Status previewDraft();
  Status publishDraft(int target = -1);

  void setSeed(uint64_t seed);                     // reseed + reset history
  void setJournalHook(JournalHook h) { hook_ = std::move(h); }

  // Strobe policy helpers for other modules: false if disableStrobing is on and the opaque mapping/preset JSON
  // declares strobe/flash behaviour ("strobe":true / "flash":true / "type":"strobe").
  bool mappingAllowed(const std::string& mappingJson) const;

private:
  struct Pending { Action a; Trigger kind = Trigger::Time; Ns at = 0; };
  // --- state ---
  Options opt_;
  ShowDocument doc_;
  JournalHook hook_;
  uint64_t rev_ = 1; bool dirty_ = false;
  Ns now_ = 0, evt_ = 0; bool haveNow_ = false;   // now_ = last update() time; evt_ = event timestamp (taps, quantize)
  ShowMode mode_ = ShowMode::Auto; bool paused_ = false, override_ = false; int modeRestore_ = -1;
  int scene_ = -1, outgoing_ = -1; Ns transStart_ = 0; Ns transNs_ = 0; std::string transType_; float transProg_ = 1.0f;
  std::vector<OutputState> outs_;
  std::vector<int> outSceneOverride_;    // per output; -1 follows program
  int target_ = -1, bank_ = 0;
  bool vBlack_ = false, lBlack_ = false, eBlack_ = false, strobeOff_ = false, armReq_ = false;
  // cues
  int cue_ = -1; bool cueEnded_ = false; Ns cueElapsed_ = 0; double cueBeats_ = 0; bool cueWaitTempo_ = false;
  bool cueMediaMissing_ = false; int pendingCueIdx_ = -1; Ns pendingCueAt_ = 0;
  // autonomous
  uint64_t seed_ = 1; uint64_t rng_ = 1; Ns dwell_ = 0; Ns dwellTarget_ = 0; Ns autoWaitUntil_ = -1;
  std::vector<int> history_;
  // tempo / key
  bool bpmManual_ = false; double bpmVal_ = 0; Estimate::Source bpmSrc_ = Estimate::Manual; Ns bpmSetNs_ = 0;
  std::vector<Ns> taps_;
  bool keyManual_ = false; int keyPc_ = 0; bool keyMinor_ = false; Ns keySetNs_ = 0;
  FeatureSnapshot feat_; Estimate effTempo_, effKey_;
  bool anchorValid_ = false; Ns anchor_ = 0; double anchorBpm_ = 0;
  // audio
  bool haveSeq_ = false; uint64_t lastSeq_ = 0; Ns lastSeqNs_ = 0; bool explicitLost_ = false, audioLost_ = false;
  bool idle_ = false, idleFromAudio_ = false;
  // text
  std::string draft_, preview_;
  std::vector<Pending> pending_;
  Ns lastTkJournal_ = -1;

  // --- helpers ---
  void resetRuntime();
  void emit(const char* kind, const std::string& json);
  void commit();
  ActionResult reply(bool ok, const std::string& why);
  ActionResult apply(const Action& a, bool fromPending);
  int sceneIndexById(const std::string& id) const;
  bool sceneAllowed(int idx) const;
  int stepScene(int dir) const;
  void setScene(int idx, const Transition& tr, const char* reason);
  void startDwell();
  int pickNext();
  void rngSeed(uint64_t s);
  uint64_t rngNext();
  bool beginCue(int idx, const char* reason);
  void endCueRelease();
  Ns nextBoundary(Ns from, bool bar) const;
  void mergeFeatures();
  bool tempoValid() const { return effTempo_.valid && effTempo_.value > 0; }
  bool automationRunning() const { return !paused_ && !override_ && !idle_; }
  void tickAuto(Ns dt);
  void tickCues(Ns dt);
  void runPending();
  void operatorOverride(const char* why);
  void resolveTargets(int t, std::vector<int>& out) const;
  void publishTo(int target, const std::string& id, const std::string& text);
  void resume(bool next);
  ActionResult tap(Ns t);
  int effTarget(const Action& a) const { return a.target == -1 ? target_ : a.target; }
  bool beginFirstAvailable(int from, int dir, const char* reason);
  void clearManualIdle() { if (!idleFromAudio_) idle_ = false; }
  void enterMode(ShowMode m);
  ActionResult submitInner(const Action& a);
};

} // namespace show
} // namespace ss
