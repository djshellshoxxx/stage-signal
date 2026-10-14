// AppController (MOD-01 backend): the single process-wide orchestrator of spec §8. Portable; every platform piece is injected
// (AppDeps). The UI layer talks ONLY to this class and polls immutable snapshots; it never touches DSP/render/devices directly.
//
// Threads owned (spec §8 ownership):
//   analysis   capture callback -> bounded queue (250 ms) -> Analyzer (raw audio) + Dynamics (recording/broadcast bus only)
//              -> FeatureSnapshot mailbox; processed audio -> recording / broadcast-feeder queues (2 s)
//   render     elapsed-time paced (base 25 FPS) -> per logical output Renderer -> newest-wins frame mailbox per output; reduced-rate
//              operator preview; feeds recording (video) and the broadcast feeder
//   control    ~60 Hz: sole owner of ShowEngine. Drains the action queue (256) and the critical latch, merges MIDI, publishes ShowView
//   lighting   25 Hz: LightingEngine fed from ShowView; DRY-RUN unless lighting was explicitly armed
//   cast       broadcast tick + encode feeder (only while a broadcast session is active)
//   io         persistence / bundles / benchmark (never the control thread)
// Callbacks (audio, display, camera, MIDI, recording, broadcast) only copy into bounded queues/mailboxes/atomics.
//
// Safety invariants (tested): recording, broadcast, DMX and camera publication are never started or re-armed by load, restart,
// crash recovery, device reconnect, setup or benchmark. Broadcast needs a UserStartToken. No datagram leaves via the real
// lighting sink before an explicit Arm. Audio is never switched to another device automatically.
#pragma once
#include "core/app/app_types.h"
#include "core/app/output_router.h"
#include "persistence/bundle.h"
#include "persistence/show_io.h"
#include "perf/benchmark.h"
#include "diagnostics/support_bundle.h"
#include "diagnostics/event_journal.h"
#include <future>
#include <set>

namespace ss { namespace app {

using AckCallback = std::function<void(const ActionResult&)>;

struct BenchmarkRequest {
  std::set<BenchId> scenarios = {BenchId::BEN01};   // BEN-01 by default; others run on mock sinks only
  double durationScale = 1.0;                       // < 1: scaled run (flagged non-qualifying)
  bool paceRealTime = true;                         // false: tests/dry runs step a FakeClock without sleeping
  FakeClock* fakeClock = nullptr;                   // required when paceRealTime == false
  int width = 800, height = 600;                    // composition size
};
struct BenchmarkOutcome { Status status; BenchResult result; std::string jsonPath; BenchProfile profile; };

class AppController {
 public:
  AppController(const AppConfig& cfg, const AppDeps& deps);
  ~AppController();                                  // runs shutdown() if needed
  AppController(const AppController&) = delete; AppController& operator=(const AppController&) = delete;

  // Loads LocalSettings, detects crash recovery, binds saved routes/devices, starts the worker threads (unless manualPump).
  // Never opens DMX/recording/broadcast/camera. Idempotent.
  Status start();
  // Ordered stop (spec §8): disarm outward -> broadcast/recording -> quiesce MIDI+lighting -> capture/camera -> drain tasks ->
  // workers -> renderer -> UI close. Each stage has a timeout and a diagnostic result. Idempotent.
  ShutdownReport shutdown();
  bool running() const;
  // manualPump only: runs one cycle of every worker at the injected clock's current time (deterministic tests).
  void pump();

  // ---- actions (IF-02). Thread-safe. The future is satisfied on the control thread (Accepted/Rejected + reason + revision).
  // Queue full (256) -> immediate Rejected "action queue full". Critical actions (blackouts, disarm, stops) use a separate latch.
  std::future<ActionResult> submit(const Action& a);
  void submit(const Action& a, AckCallback ack);     // ack runs on the control thread (or inline when rejected at the door)
  // Broadcast start with an explicit operator token (UserActionGate in broadcast/session.h). Settings come from configureBroadcast().
  UserActionGate& startGate();
  Status startBroadcast(UserStartToken&& token, const PublicationInputs& pub = PublicationInputs());
  void stopBroadcast();

  // ---- show lifecycle (IF-11, REQ-S13/S14/X01). Invalid input never replaces the running show.
  std::future<Status> applyDocument(const show::ShowDocument& doc);          // validated; edits and "new show"
  std::future<Status> openShow(const std::string& path);                     // loadWithFallback; media root = file's directory
  std::future<Status> saveShow(const std::string& path);                     // "" = current path; atomic, retained generations
  std::future<Status> exportPortable(const std::string& zipPath, const persist::ExportOptions& opt = persist::ExportOptions());
  std::future<Status> importPortable(const std::string& archivePath, const std::string& destDir);
  persist::RecoveryInfo recoveryInfo() const;                                // previous session crashed? autosave valid?
  std::future<Status> restoreRecovery(const std::string& desiredPath);       // loads autosave into a copy; idle + everything disarmed
  Status discardRecovery();
  std::shared_ptr<const show::ShowDocument> document() const;                // current document (immutable)
  show::ShowSnapshot showSnapshot() const;
  bool showDirty() const;
  // text pipeline (REQ-S05): editing never reaches audience outputs; only publish does
  std::future<Status> setDraftText(const std::string& utf8);
  std::future<Status> previewDraftText();
  std::future<Status> publishDraftText(int target = -1);

  // ---- status & frames (all thread-safe, non-blocking)
  AppStatus status() const;
  std::vector<OutputRouteView> outputRoutes() const;
  bool outputFrame(const std::string& logicalId, FramePtr& out) const;       // newest completed frame of an output (empty if none/disconnected)
  bool previewFrame(FramePtr& out) const;                                    // operator preview (reduced rate, follows blackout)
  bool cameraPreviewFrame(FramePtr& out) const;                              // selected camera, regardless of publication
  MetricsSnapshot metrics();
  FeatureSnapshot features() const;                                          // raw-audio analysis features (meters)

  // ---- audio (REQ-A01/A02/A10)
  std::vector<AudioDeviceInfo> audioDevices();
  Status selectAudio(const AudioOpenParams& p);                              // explicit operator choice; saved in LocalSettings
  Status reopenAudio();                                                      // reopen the SAME device only
  void closeAudio();
  void setDynamics(const DynamicsConfig& c);                                 // recording/broadcast bus (REQ-R02)
  DynamicsConfig dynamics() const;
  void acknowledgeGainNote(bool ack);                                        // setup: hardware gain is the operator's job

  // ---- displays / routes (MOD-05)
  std::vector<DisplayInfo> displays();
  void refreshDisplays();                                                    // re-enumerate now (also done on the change callback)
  Status assignOutput(const std::string& logicalId, const DisplayInfo& d);   // operator route selection (works for identical monitors)
  Status assignOutputById(const std::string& logicalId, const std::string& displayId);
  Status unassignOutput(const std::string& logicalId);
  Status setControlsDisplay(const std::string& displayId);
  void setOutputDiagnostics(const std::string& logicalId, bool on);          // audience diagnostics are OFF by default
  Status setProgramOutput(const std::string& logicalId);                     // output recorded/broadcast ("" = first Program, else first Audience)

  // ---- camera (MOD-08). Preview and publication are separate: selecting a camera NEVER adds it to audience/broadcast scenes.
  std::vector<CameraInfo> cameras();
  Status selectCamera(const CameraInfo& c, CameraFormat want = CameraFormat());   // preview only
  void closeCamera();
  void setCameraTreatments(const CameraTreatments& t);
  void publishCamera(const CameraPublication& p);                            // explicit publication (to outputs; empty list = all)
  CameraPublication cameraPublication() const;

  // ---- media (MOD-07)
  void setMediaRoot(const std::string& dir);
  std::vector<std::string> missingMedia() const;                             // asset hashes that failed to load (placeholders shown)

  // ---- MIDI (MOD-09)
  std::vector<MidiPortInfo> midiPorts();
  Status selectMidiInput(const MidiPortInfo& p);
  Status selectMidiOutput(const MidiPortInfo& p);
  Status reconnectMidi();                                                    // explicit operator action after a lost port
  MappingTable& midiMappings();                                              // mapping table (learn/edit/JSON); changes mark the show dirty
  MidiRouteGraph& midiRoutes();
  void setMidiClockEnabled(bool on);                                         // 24 ppqn from the effective BPM
  void setMidiFeedback(const std::vector<MidiFeedbackRule>& rules);         // action -> MIDI out (never for MIDI-originated actions)
  void setMidiOutConfig(const MidiOutConfig& c);

  // ---- lighting (MOD-10). Preview/setup/benchmark are dry-run only; Arm is an explicit action (ActionId::ArmLighting).
  Status setLightingInterface(const LightingInterface& li);                  // setup; validated, saved; never transmits
  void setLightingLooks(const std::map<std::string, ss::Cue>& looks);       // opaque cue ref -> lighting cue
  Status setLightingAudioMappings(const std::vector<AudioMapping>& m);
  uint64_t lightingDatagramsSent() const;                                    // via the real sink (0 until armed)
  uint64_t lightingDatagramsPreviewed() const;                               // dry-run "would send"
  bool lightingArmed() const;

  // ---- recording / broadcast configuration (start goes through actions / startBroadcast)
  Status setRecordingFolder(const std::string& dir);
  void configureRecording(const RecordingProfile& p);
  Status configureBroadcast(const BroadcastSettings& s);
  BroadcastSettings broadcastSettings() const;
  Status storeSecret(const std::string& label, const std::string& secret);  // ISecretStore only; LocalSettings keeps the reference
  RecordingStatus recordingStatus() const;
  BroadcastStatus broadcastStatus() const;

  // ---- setup wizard (UJ-01), saved as LocalSettings
  SetupState setupState() const;
  Status saveSetup();
  persist::LocalSettings localSettings() const;

  // ---- benchmark (REQ-P01/P02): mock sinks only; refused while recording/broadcast/DMX are live
  std::future<BenchmarkOutcome> runBenchmark(const BenchmarkRequest& req = BenchmarkRequest());

  // ---- adaptation / admission
  void setQualityLocked(bool locked);
  CapabilityStatus capability(const std::vector<FeatureDef>& requested = defaultFeatureCatalog());

  // ---- diagnostics
  uint64_t addMarker(const std::string& label);                              // journal + recording marker
  std::vector<JournalEvent> journalSnapshot() const;
  Status exportSupportBundle(const std::string& dest, const std::set<std::string>& approved, BundleReport* rep = nullptr);
  std::vector<BundleCategory> supportBundlePreview();

 private:
  struct Impl;
  std::shared_ptr<Impl> p_;
};

}} // namespace ss::app
