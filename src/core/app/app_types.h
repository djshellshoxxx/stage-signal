// App integration layer value types (spec §8, §11): configuration, injected ports, status snapshots, setup state.
// Portable C++17. NO Windows headers: every platform piece reaches the controller through the ports in core/ports.h.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include "core/show/model.h"
#include "core/show/engine.h"
#include "core/dynamics/dynamics.h"
#include "lighting/engine.h"
#include "midi/mapping.h"
#include "media/decoder_registry.h"
#include "perf/adaptation.h"
#include "perf/metrics.h"
#include "persistence/file_ops.h"
#include "diagnostics/file_ops.h"
#include "recording/io.h"
#include "recording/video_encoder.h"
#include "recording/session.h"
#include "broadcast/session.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ss { namespace app {

using FramePtr = std::shared_ptr<const Frame>;

// ---- configuration --------------------------------------------------------------------------------------------
struct AppConfig {
  std::string dataDir = "stagesignal-data";   // settings.json, autosave, recovery marker, logs/, bench/
  std::string mediaRoot;                      // doc.media[].relPath resolves against this
  bool manualPump = false;                    // tests: no threads are started; the caller drives pump()
  bool autoOpenSavedAudio = true;             // start(): reopen the SAVED audio device id (never any other device)
  // rates (BASE target 25 FPS render; operator preview is reduced)
  double renderFps = 25.0;
  double previewFps = 10.0;
  int previewW = 320, previewH = 240;         // operator preview composition size
  int programW = 800, programH = 600;         // size of a virtual Program output (record/broadcast) without a display
  int maxInternalPixels = 800 * 600;          // internal render scale is derived so w*h*scale^2 <= this
  double controlHz = 60.0, lightingHz = 25.0;
  Ns adaptationPeriodNs = 250 * kNsPerMs;
  // bounded queues (spec §8)
  Ns captureQueueNs = 250 * kNsPerMs;         // capture -> analysis
  Ns consumerAudioQueueNs = 2 * kNsPerSec;    // processed audio -> each consumer (broadcast feeder)
  size_t actionQueueCap = 256;                // UI/MIDI/show actions
  Ns cameraFreezeNs = 500 * kNsPerMs;         // lost camera: freeze last frame this long, then hide the layer (REQ-S11)
  Ns autosaveInterval = 30 * kNsPerSec;
  Ns audioStatusMaxLatencyNs = kNsPerSec;     // REQ-A02
  // lighting (preview/setup/benchmark always dry-run; the real sink is only used after an explicit Arm action)
  EngineConfig lighting;
  // adaptive quality (BASE: 25 FPS scene rate => ladder 25->20->15 is skipped to 20->15)
  AdaptationConfig adaptation;
  bool qualityLocked = false;
  // shutdown
  int shutdownStageTimeoutMs = 2000;
  // dynamics runs only while recording/broadcast needs the processed bus unless this is set (meters of the bus)
  bool dynamicsAlwaysOn = false;
  AppConfig() {
    adaptation.baseSceneFps = 25;
    adaptation.previewFpsNormal = 10;
    adaptation.previewFpsReduced = 5;
    adaptation.sourceRequestSupported = false;   // camera/decoder renegotiation not wired: step is skipped, not faked
  }
};

struct SystemInfo {                             // supplied by the platform layer; recorded verbatim in benchmark results
  std::string os = "unknown", powerPlan = "unknown", cpu = "unknown", gpu = "unavailable", gpuDriver = "unavailable",
              appHash = "dev", monitorTopology = "unknown";
};

// Every platform piece is injected. Pointers are borrowed and MUST outlive the AppController (and any thread it started;
// shutdown() joins them, so destroying the controller first is sufficient). nullptr = capability absent.
struct AppDeps {
  IClock* clock = nullptr;                      // nullptr -> systemClock()
  IAudioCapture* audio = nullptr;
  IDisplayProvider* displays = nullptr;
  ICamera* camera = nullptr;
  ITextRasterizer* text = nullptr;
  IMidiPort* midiIn = nullptr;
  IMidiPort* midiOut = nullptr;                 // may be the same object as midiIn
  IDatagramSink* lightingSink = nullptr;        // the ONLY real network sink; used only while lighting is armed
  ISecretStore* secrets = nullptr;
  std::function<std::unique_ptr<IVideoDecoder>()> makeVideoDecoder;   // optional; registered in the media decoder registry
  persist::IFileOps* files = nullptr;           // persistence port (show/settings/bundles); nullptr -> persist::defaultFileOps()
  IFileOps* diagFiles = nullptr;                // diagnostics port (logs/journal/support bundle); nullptr -> stdFileOps()
  IFileSystem* recFs = nullptr;                 // recording file system (nullptr -> stdFileSystem())
  IDiskInfo* disk = nullptr;                    // recording free-space probe (nullptr: recording unavailable)
  const EncoderRegistry* encoders = nullptr;    // nullptr -> built-ins
  TransportFactory* transports = nullptr;       // broadcast transports (nullptr: broadcast unavailable)
  ICpuCounters* cpu = nullptr;                  // adaptation / benchmark counters (nullptr: reported unavailable)
  IMemCounters* mem = nullptr;
  SystemInfo sys;
};

// ---- output routing (MOD-05) ----------------------------------------------------------------------------------
enum class RouteState : uint8_t {
  Unassigned,     // audience output with no display chosen: renders nothing
  Active,         // bound to a present, unambiguous display (never the controls display)
  Disconnected,   // assigned display is gone; waits for the SAME identity to return
  NeedsReassign,  // identical monitors: stays idle until the operator re-confirms the route
  Blocked,        // would cover the controls display / duplicate another route: refused
  Virtual,        // Program output without a window (record/broadcast composition)
  ControlsHost    // role Controls: hosts operator panels, no frames
};
const char* routeStateName(RouteState s);

struct OutputRouteView {                        // one row per logical output (UI route table)
  std::string logicalId;
  show::OutputRole role = show::OutputRole::Audience;
  std::string displayId, displayName;           // stable identity (never an index) + friendly name
  int x = 0, y = 0, w = 0, h = 0, rotation = 0; // current geometry of the bound display (0 if none)
  RouteState state = RouteState::Unassigned;
  std::string note;                             // operator-facing explanation ("display disconnected; controls unaffected")
  uint64_t generation = 0;                      // changes on every rebind/geometry change; windows must be re-created
  bool independentScene = false;                // false = duplicates the program composition
  bool diagnostics = false;                     // audience diagnostics (reference border) explicitly enabled
  bool blackedOut = false;                      // final composition currently forced to black
  std::string identifyLabel;                    // operator-triggered label text for this output's display
  uint64_t framesPublished = 0, framesDropped = 0;
};

// ---- status ---------------------------------------------------------------------------------------------------
struct Warning { std::string id; std::string text; bool critical = false; };

struct AppStatus {
  uint64_t seq = 0;
  // audio (top bar)
  std::string audioDevice;                      // chosen device name ("" none)
  AudioState audioState = AudioState::Closed;
  std::string audioDetail;
  float meterRmsDb[2] = {kDbSilence, kDbSilence}, meterPeakDb[2] = {kDbSilence, kDbSilence};
  bool inputNearFullScale = false, possibleClipping = false;
  bool audioLossPolicyEngaged = false;          // show engine reports audio lost (continue/idle per policy)
  uint64_t captureOverruns = 0;
  // mode / tempo / key
  show::ShowMode mode = show::ShowMode::Auto;
  bool paused = false, manualOverride = false;
  bool bpmValid = false; double bpm = 0; float bpmConfidence = 0; std::string bpmLabel = "Unknown";   // Manual | Tap | Estimated | Unknown
  bool keyValid = false; std::string keyName = "Unknown"; std::string keyLabel = "Unknown";            // e.g. "A minor"
  // camera
  CameraState cameraState = CameraState::Closed; std::string cameraName; bool cameraPublished = false;
  // DMX / REC / LIVE
  bool dmxArmed = false; std::string dmxText = "Off";
  RecState recState = RecState::Idle; double recElapsedSec = 0;
  BroadcastState liveState = BroadcastState::Off; std::string liveText = "Off";
  // latches
  bool visualBlackout = false, lightingBlackout = false, emergencyBlackout = false, strobeDisabled = false;
  bool idleActive = false;
  // outputs
  std::vector<OutputRouteView> outputs;
  // adaptation
  int adaptationLevel = 0; std::string adaptationStep = "none"; bool qualityLocked = false;
  double sceneFps = 25, previewFps = 10, renderScale = 1.0;
  // show
  std::string showName; uint64_t showRevision = 0; bool showDirty = false; bool recovered = false;
  int sceneIndex = -1; std::string sceneId;
  // MIDI
  bool midiInputAlive = false, midiOutputAlive = false; size_t midiMappings = 0, midiMappingsUnavailable = 0;
  // media
  size_t mediaMissing = 0;
  // queue health (bounded everywhere)
  size_t actionQueueDepth = 0, captureQueueDepth = 0, castAudioQueueDepth = 0;
  uint64_t actionsRejectedFull = 0, captureDrops = 0, castAudioDrops = 0;
  uint64_t framesRendered = 0, deadlinesMissed = 0;
  std::vector<Warning> warnings;
};

// ---- setup wizard model (UJ-01) -------------------------------------------------------------------------------
enum class SetupStepId : uint8_t { Displays, Audio, Gain, Camera, Midi, Lighting, Recording, Benchmark, Review };
struct SetupStep { SetupStepId id; std::string title; bool complete = false; bool optional = false; std::string summary; };
struct LightingInterface {
  LightProtocol protocol = LightProtocol::ArtNet; std::string host, nicAddr; bool sacnMulticast = false, allowBroadcast = false;
};
struct SetupState {
  struct DisplayRole { std::string logicalId; show::OutputRole role = show::OutputRole::Audience; std::string displayId; RouteState state = RouteState::Unassigned; };
  std::vector<DisplayRole> displayRoles;
  std::string controlsDisplayId;
  std::string audioDeviceId, audioDeviceName; int audioChannelRoute = 0;
  std::string gainNote = "Set the input gain on the audio interface or mixer (hardware). StageSignal never changes hardware gain: "
                         "adjust it until peaks stay below -6 dBFS and the clip indicator never lights.";
  bool gainAcknowledged = false;
  std::string cameraId; bool cameraPreviewed = false;
  std::string midiInId, midiOutId; size_t midiMappings = 0;
  LightingInterface lighting; bool lightingConfigured = false;
  std::string recordingFolder;
  bool benchmarkDone = false; std::string benchmarkProfileHash; bool benchmarkQualifying = false;
  std::vector<SetupStep> steps;
  bool complete() const { for (const auto& s : steps) if (!s.optional && !s.complete) return false; return true; }
};

// ---- shutdown (spec §8 stop order) ------------------------------------------------------------------------------
struct ShutdownStage { std::string name; bool ok = true; bool timedOut = false; int64_t elapsedMs = 0; std::string detail; };
struct ShutdownReport { std::vector<ShutdownStage> stages; bool clean = true; };

// ---- feedback / options ----------------------------------------------------------------------------------------
struct CameraPublication { bool published = false; std::vector<std::string> outputs; /* logical ids; empty = every audience/program output */ };
struct MidiFeedbackRule {                       // action -> MIDI output (never for MIDI-originated actions)
  ActionId action = ActionId::None; MidiMsgType type = MidiMsgType::Note; int channel = 0, number = 0; int valueOn = 127, valueOff = 0;
};

}} // namespace ss::app
