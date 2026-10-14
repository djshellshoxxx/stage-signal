// INTERNAL: AppController::Impl. Not part of the UI-facing API (use controller.h).
#pragma once
#include "core/app/controller.h"
#include "core/app/render_compose.h"
#include "core/analysis/analyzer.h"
#include "core/queue.h"
#include "core/security.h"
#include "diagnostics/logger.h"
#include "diagnostics/rotating_file.h"
#include "diagnostics/retention.h"
#include "lighting/mock_sink.h"
#include "media/manager.h"
#include "midi/output.h"
#include "midi/router.h"
#include "perf/admission.h"
#include "render/renderer.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ss { namespace app {

template <class T> class SharedSlot {
 public:
  std::shared_ptr<T> get() const { std::lock_guard<std::mutex> l(m_); return p_; }
  void set(std::shared_ptr<T> p) { std::lock_guard<std::mutex> l(m_); p_ = std::move(p); }
 private:
  mutable std::mutex m_; std::shared_ptr<T> p_;
};

// Time-bounded block queue (capacity expressed in audio time, spec §8). push() never blocks; over-capacity => dropped + counted.
class AudioQueue {
 public:
  explicit AudioQueue(Ns window) : window_(window) {}
  bool push(const AudioBlockPtr& b) {
    std::lock_guard<std::mutex> l(m_);
    uint64_t limit = (uint64_t)b->rate * (uint64_t)(window_ / 1000000) / 1000u;
    if (!q_.empty() && frames_ + b->frames() > limit) { ++drops_; droppedFrames_ += b->frames(); return false; }
    q_.push_back(b); frames_ += b->frames(); limit_ = limit; if (q_.size() > high_) high_ = q_.size(); return true;
  }
  bool pop(AudioBlockPtr& out) {
    std::lock_guard<std::mutex> l(m_);
    if (q_.empty()) return false;
    out = std::move(q_.front()); q_.pop_front(); frames_ -= out->frames(); return true;
  }
  size_t blocks() const { std::lock_guard<std::mutex> l(m_); return q_.size(); }
  double fraction() const { std::lock_guard<std::mutex> l(m_); return limit_ ? (double)frames_ / (double)limit_ : 0.0; }
  uint64_t drops() const { std::lock_guard<std::mutex> l(m_); return drops_; }
  uint64_t frames() const { std::lock_guard<std::mutex> l(m_); return frames_; }
  size_t highWater() const { std::lock_guard<std::mutex> l(m_); return high_; }
  void clear() { std::lock_guard<std::mutex> l(m_); q_.clear(); frames_ = 0; }
 private:
  mutable std::mutex m_; std::deque<AudioBlockPtr> q_; Ns window_; uint64_t frames_ = 0, limit_ = 0, drops_ = 0, droppedFrames_ = 0; size_t high_ = 0;
};

struct Task {
  std::function<void(Ns)> run;                       // executes on the control thread
  std::function<void(const std::string&)> cancel;    // queue full / shutting down
  Ns enq = 0;
  bool ack = false;                                  // record UI ack latency
};

struct FrameBox {
  Mailbox<FramePtr> box;
  std::atomic<uint64_t> published{0}, dropped{0};
};

struct OutSlot {
  std::string id; int w = 0, h = 0; uint64_t gen = 0; float baseScale = 1.0f;
  std::unique_ptr<Renderer> r;
  SceneClocks clocks;
  std::vector<std::shared_ptr<Frame>> pool;
  bool ok = false;
};

struct AppController::Impl {
  AppConfig cfg; AppDeps d; IClock* clk;
  explicit Impl(const AppConfig& c, const AppDeps& dd);
  ~Impl();

  // ---- lifecycle
  std::atomic<bool> started{false}, stopping{false}, shutDone{false};
  std::atomic<bool> stopAnalysis{false}, stopRender{false}, stopControl{false}, stopLight{false}, stopCast{false}, stopIo{false}, cancelBench{false};
  ShutdownReport lastShutdown;
  std::shared_ptr<Impl> selfHold;                    // threads capture this; reset by shutdown
  std::mutex lifeMu;
  std::thread thAnalysis, thRender, thControl, thLight, thCast, thIo;
  std::atomic<bool> exAnalysis{true}, exRender{true}, exControl{true}, exLight{true}, exCast{true}, exIo{true};
  std::condition_variable wakeCv; std::mutex wakeMu; std::atomic<uint64_t> wakeSeq{0};
  void wake() { { std::lock_guard<std::mutex> l(wakeMu); ++wakeSeq; } wakeCv.notify_all(); }
  Status start(const std::shared_ptr<Impl>& self);
  ShutdownReport shutdown();
  void sleepUntil(Ns t, const std::atomic<bool>& stopFlag);   // real sleep slices honouring the flag
  bool joinWithin(std::thread& t, std::atomic<bool>& exited, int timeoutMs);
  int64_t nowMs() const { return (int64_t)(clk->nowNs() / kNsPerMs); }

  // ---- diagnostics (all access through diagMu)
  std::mutex diagMu;
  std::unique_ptr<RotatingFile> logFile, journalFile;
  std::unique_ptr<Logger> logger; std::unique_ptr<EventJournal> journal; sec::Redactor redactor;
  IFileOps* dfiles = nullptr;
  void logInfo(const std::string& c, const std::string& m);
  void logWarn(const std::string& c, const std::string& m);
  void jrec(JournalKind k, const std::string& subject, const std::string& detail = std::string(), double a = 0, double b = 0);
  std::mutex warnMu; std::map<std::string, Warning> warns;
  void setWarning(const std::string& id, const std::string& text, bool critical);
  void clearWarning(const std::string& id);

  // ---- persistence
  persist::IFileOps* pfiles = nullptr;
  std::mutex setMu; persist::LocalSettings settings;
  std::unique_ptr<persist::CrashRecovery> crash; persist::RecoveryInfo recov; bool recovered = false;
  std::mutex pathMu; std::string currentPath, mediaRoot;
  std::atomic<bool> docDirty{false}; Ns dirtySince = -1;
  BoundedQueue<std::function<void()>> ioQ{64};
  std::mutex ioMu; std::condition_variable ioCv;
  bool postIo(std::function<void()> f);
  void ioLoop();
  size_t ioStep();                                   // manualPump: run queued io inline
  Status saveSettingsNow();
  void settingsPut(const std::string& k, const std::string& v);
  std::string settingsGet(const std::string& k) const;

  // ---- show (control thread owned)
  std::unique_ptr<show::ShowEngine> engine;
  std::shared_ptr<const show::ShowDocument> docPtr; std::mutex docMu;     // docPtr readable from any thread under docMu
  uint64_t docGen = 0;
  Mailbox<ShowViewPtr> viewBox;
  BoundedQueue<Task> actQ{256}, critQ{32};
  std::atomic<uint64_t> actRejectedFull{0};
  Ns lastUpdate = -1, nextAdapt = 0, nextSec = 0;
  bool lastAudioAvailSent = true; bool audioAvailInit = false;
  void controlLoop();
  void controlStep(Ns now);
  void submitAction(const Action& a, AckCallback ack);
  std::future<Status> postStatus(std::function<Status(Ns)> fn);
  bool pushTask(std::function<void(Ns)> run, std::function<void(const std::string&)> cancel);   // false: queue full / shutting down
  std::thread thBench; std::atomic<bool> exBench{true};
  std::string lastBenchJson; std::mutex benchMu;
  ActionResult routeAction(const Action& a, Ns now);
  void publishView(Ns now);
  Status adoptDocument(const show::ShowDocument& doc, bool recovery, bool markDirty);
  void onShowJournal(const std::string& kind, const std::string& json);
  void housekeeping(Ns now);
  void adaptationTick(Ns now);
  void refreshDisplaysNow();
  std::atomic<bool> displaysDirty{false};
  show::ShowDocument defaultDocument() const;
  show::ShowSnapshot lastSnap; FeatureSnapshot lastEff;

  // ---- audio / analysis
  AudioQueue capQ{250 * kNsPerMs}; AudioQueue castQ{2 * kNsPerSec};
  std::atomic<uint64_t> capOverruns{0}; std::atomic<bool> pendingDisc{false};
  std::unique_ptr<Analyzer> analyzer; std::unique_ptr<Dynamics> dynamics; uint32_t aRate = 0, aCh = 0;
  Mailbox<FeatureSnapshot> featBox; Mailbox<AudioStatus> audioStatBox;
  std::atomic<bool> audioAvail{false}, audioAvailDirty{false};
  std::mutex audioMu; AudioOpenParams audioParams; std::string audioName; bool audioChosen = false;
  std::mutex dynMu; DynamicsConfig dynCfg; std::atomic<bool> dynDirty{false};
  std::atomic<float> procPeakDb{kDbSilence}, lastGrDb{0};
  SharedSlot<RecordingSession> rec; SharedSlot<BroadcastSession> bc;
  std::atomic<bool> castActive{false};
  Ns lastBlockWallNs = 0;
  std::condition_variable anCv; std::mutex anMu;
  void analysisLoop();
  size_t analysisStep(int maxBlocks);
  void onAudioBlock(AudioBlockPtr b);
  void onAudioStatus(const AudioStatus& s);
  Status openAudio(const AudioOpenParams& p, bool save);

  // ---- output routing / render
  OutputRouter router;
  std::mutex frameMu; std::map<std::string, std::shared_ptr<FrameBox>> frameBoxes;
  Mailbox<FramePtr> previewBox, camPreviewBox, progBox;
  std::shared_ptr<FrameBox> boxFor(const std::string& id);
  std::map<std::string, OutSlot> slots; uint64_t slotsGen = ~0ull;      // render thread only
  std::unique_ptr<Renderer> previewR; SceneClocks previewClocks; std::vector<std::shared_ptr<Frame>> previewPool;
  Ns nextRender = 0, nextPreview = 0, showStartNs = 0; bool renderInit = false; uint64_t lastDocGen = ~0ull;
  std::string curMediaRef; Ns curMediaStart = 0;
  std::mutex progMu; std::string programOutput;
  std::atomic<int> qLevel{0}, sceneFpsX100{2500}, previewFpsX100{1000}, scaleX1000{1000}; std::atomic<bool> trailsReduced{false};
  std::atomic<uint64_t> framesRendered{0}, deadlinesEval{0}, deadlinesMissed{0};
  std::atomic<uint64_t> framesDroppedPool{0};
  MetricsRecorder metrics;
  void renderLoop();
  void renderStep(Ns now);
  void syncSlots(const RouteTable& t);
  FramePtr renderOne(OutSlot& s, const RenderState& rs, Ns now);
  std::shared_ptr<Frame> acquire(std::vector<std::shared_ptr<Frame>>& pool, int w, int h);
  std::shared_ptr<const Frame> mediaFrame(const std::string& hash, Ns now, Ns startNs);
  std::string programId(const ShowView& v) const;

  // ---- camera
  std::mutex camMu; CameraPublication camPub; CameraTreatments camT; CameraInfo camInfo; bool camOpen = false;
  Mailbox<CameraStatus> camStatBox;
  std::shared_ptr<Frame> camFrame; Ns camLastNs = -1;                  // render thread only
  void pollCamera(Ns now, bool published);

  // ---- media
  std::unique_ptr<media::MediaManager> mediaMgr; media::DecoderRegistry decoders;
  struct HandleEntry { media::MediaHandlePtr h; Ns tried = 0; };
  std::mutex mediaMu; std::map<std::string, HandleEntry> handles;
  media::MediaHandlePtr handleFor(const std::string& hash);
  void registerMedia(const show::ShowDocument& doc);

  // ---- MIDI
  MappingTable table; MidiRouteGraph graph; std::unique_ptr<MidiRouter> mrouter; std::unique_ptr<MidiOutputScheduler> mout;
  int midiInIdx = -1; std::string midiInId, midiOutId;
  std::atomic<bool> midiInLost{false}, midiOutLost{false}, midiOutOpen{false};
  std::atomic<double> effBpm{0};
  std::mutex fbMu; std::vector<MidiFeedbackRule> fbRules; MidiOutConfig moutCfg; bool midiClock = false;
  std::string lastMidiJson;
  void restoreSavedMidi();
  Status openMidiInput(const MidiPortInfo& p, bool save);
  Status openMidiOutput(const MidiPortInfo& p, bool save);
  void midiFeedback(const Action& a, double value);
  void syncMidiToDoc(show::ShowDocument& doc) const;
  void loadMidiFromDoc(const show::ShowDocument& doc);

  // ---- lighting
  std::mutex lightMu; std::unique_ptr<LightingEngine> dry, live; ProfileLibrary lib; Patch patch; PatchReport patchReport; bool patchOk = false;
  EngineConfig lcfg; std::map<std::string, ss::Cue> looks; std::vector<AudioMapping> audioMaps; std::string lastLookRef;
  Ns nextLight = 0; std::atomic<bool> liveArmed{false}; std::atomic<uint64_t> previewDatagrams{0};
  std::atomic<bool> liveFaulted{false};
  void lightingLoop();
  void lightingStep(Ns now);
  Status buildLighting(const show::ShowDocument& doc);
  Status doArmLighting();
  void doDisarmLighting(const char* why);
  void syncLightingLatches(const show::ShowSnapshot& s);

  // ---- recording / broadcast
  std::mutex recMu; RecordingProfile recProfile; bool recProfileSet = false;
  std::mutex bcMu; BroadcastSettings bcSettings = defaultBroadcastSettings(); std::unique_ptr<UserActionGate> gate; PublicationInputs bcPub;
  std::unique_ptr<IVideoEncoder> bcEnc; Frame bcScaled; Ns bcLastVideoPts = -1; uint64_t bcProgVer = 0; std::atomic<bool> bcKeyReq{false};
  uint64_t bcSeq = 0;
  EncoderRegistry ownedEncoders; const EncoderRegistry* encoders = nullptr;
  Status doStartRecording(ActionSource src);
  Status doStopRecording();
  Status doStartBroadcast(UserStartToken&& tok, const PublicationInputs& pub);
  void doStopBroadcast();
  void castLoop();
  void castStep(Ns now);
  PublicationInputs makePublication() const;
  bool programCameraPublished() const;

  // ---- adaptation
  struct CachedCpu : ICpuCounters { CpuSample c; CpuSample sample() override { return c; } } cachedCpu;
  AdaptationController adapt; QualityState quality;
  std::vector<std::pair<Ns, std::string>> adaptHistory;

  // ---- benchmark
  std::atomic<bool> benchRunning{false};
  BenchHistory benchHistory;
  BenchmarkOutcome runBenchmarkNow(const BenchmarkRequest& req);

  // ---- status
  std::atomic<uint64_t> statusSeq{0};
  SetupState buildSetupState() const;
  AppStatus buildStatus() const;
  std::vector<OutputRouteView> routeViews() const;
};

// helpers shared across the translation units
std::string keyNameOf(const Estimate& k);
const char* sourceLabel(const Estimate& e);
bool isCriticalAction(ActionId id);

}} // namespace ss::app
