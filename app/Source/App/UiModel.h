// Contract: THE facade between the engine and every UI tab (no UI code in here). Owner: shell-integration.
// UI code (panels, status bar, strip, overlays) sees only this header: read-only snapshots, a thread-safe
// submitAction(), a command channel for non-Action requests, frame access for previews, and a polling surface.
// It is JUCE-free; UiModelPoller (App/UiModelPoller.h) is the juce::Timer that drives poll() at 30 Hz.
//
// Rules for UI code:
//   * Read state only from the snapshot you were handed (or snapshot()). Never keep pointers into the engine.
//   * Change state only with submitAction() (IF-02) or postCommand() (setup/device/file requests).
//   * Never block, never call an adapter directly. Results come back via ActionOutcome / next snapshot.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "App/ActionInbox.h"
#include "App/DiagBus.h"
#include "App/HostMode.h"
#include "App/Mailboxes.h"
#include "App/Views/BroadcastView.h"
#include "App/Views/LightingView.h"
#include "App/Views/MediaView.h"
#include "App/Views/MidiView.h"
#include "App/Views/RecordView.h"
#include "Engine/AdapterCommon.h"
#include "ss/dynamics/processor.hpp"
#include "ss/render/scene.hpp"
#include "ss/show/types.hpp"

namespace ss::app {

// ---- snapshot ---------------------------------------------------------------------------------------
struct ThreadView { std::string name; bool running = false, stale = false; std::uint64_t beats = 0; };

struct AdapterRow { std::string name; AdapterState state = AdapterState::Stopped; std::string lastError; };

struct AudioDeviceRow { std::string typeName, deviceName; int inputChannels = 0; std::vector<double> sampleRates; };

struct AudioView {
    AdapterState deviceState = AdapterState::Unavailable;   // "No audio device" is Unavailable (REQ-A02)
    std::string deviceError;
    bool signalAlive = false;                               // blocks arriving within the last 500 ms
    std::uint32_t rateHz = 0, channels = 0;
    std::uint64_t blocks = 0, droppedBlocks = 0, sanitisedSamples = 0, discontinuities = 0;
    ss::dynamics::DynamicsTelemetry dynamics;               // output meters for the Live tab
    std::vector<AudioDeviceRow> devices;                    // refreshed every few seconds by the IO thread
    std::string currentType, currentName;                   // the device the app last asked for (saved in settings)
};

struct ScenePadInfo { std::uint32_t id = 0; std::string name; bool strobe = false; };

// The show file as the operator sees it (title bar, Setup > Files, quit confirm, pads).
struct ShowFileView {
    std::string name;                          // document name
    std::string fileName;                      // base name only, never a full path
    bool dirty = false;                        // unsaved relative to the user's file
    bool autosavePending = false;
    std::string autosaveNote;                  // "Autosaved 12:30", "Autosave failed: ...", or empty
    std::string error;                         // last load/save error, sanitised
    std::vector<ScenePadInfo> scenes;          // document order; pad i of bank b is scenes[b * 8 + i]
    bool freeBytesValid = false;
    std::uint64_t freeBytes = 0;               // free space where shows and recordings go
};

// Recovery mode (app-shell 8): shown as a banner with three explicit choices. Nothing is restored without a click.
struct RecoveryView {
    bool active = false;
    std::string text;                          // what was restored and what was not
    bool autosaveAvailable = false, lastSavedAvailable = false;
    std::string autosaveTime;                  // local time of the autosave, empty when unknown
};

struct BroadcastEndpointView {
    std::string host, path;
    int port = 0;
    bool keySaved = false;                     // the key itself is never in a snapshot
};

struct OutputView {
    OutputId id = 0;
    std::string label;
    RouteKind kind = RouteKind::Audience;
    int width = 0, height = 0;
    std::uint64_t framesRendered = 0, framesDropped = 0;
    bool blackout = false;
};

struct PerfView {
    double cpuPct = 0.0;
    std::uint64_t workingSetBytes = 0;
    double renderFps = 0.0, renderMsLast = 0.0, renderMsWorst = 0.0;
    bool valid = false;
    int adaptStep = 0, adaptMax = 0;           // performance adaptation ladder (0 = full quality)
    std::string adaptNote;                     // empty at full quality
};

// Musical features wired by the shell (FEAT-001..003): phrase position, key colour, beat-look modulation.
struct MusicView {
    bool phraseLocked = false, phraseStart = false, preDrop = false;
    int barInPhrase = -1, phraseBars = 8, barsToPhraseStart = 0;
    bool keyColorValid = false;
    unsigned char keyR = 0, keyG = 0, keyB = 0;
    bool beatValid = false;
    float beatValue = 0.f;
    std::int64_t beatCycle = 0;
    bool beatCapped = false;                   // the flash ceiling reduced the look
    bool keyTintOn = false, beatLightOn = false;
};

struct UiSnapshot {
    std::uint64_t version = 0;                // strictly increasing per publish
    ss::Ns atNs = 0;                          // IClock monotonic when built (UI shows a banner when it goes stale)
    HostKind host = HostKind::Standalone;
    bool headless = false;
    bool recovery = false;                    // Recovery mode: idle scene, nothing armed (app-shell 8)
    // --- Live tab / status bar -------------------------------------------------------------------
    ss::FeatureSnapshot features;             // IF-03: tempo, key, bands, energy arc, clipping flag
    ss::render::AudioVisualInput visual;      // 32 spectrum bins + 256 waveform samples
    ss::show::ShowSnapshot show;              // IF-05 full: outputs, cue, pads, slots, undo depth, blackout
    std::vector<OutputView> outputs;
    AudioView audio;
    PerfView perf;
    MusicView music;
    ShowFileView showFile;
    RecoveryView recoveryInfo;
    BroadcastEndpointView broadcastEndpoint;
    bool secondary = false;                   // plugin Secondary instance: analysis and UI only, outputs disabled
    std::vector<std::pair<std::string, std::string>> prefs;   // appearance, wizard, display roles ("ui.*" keys)
    // --- other tabs (struct definitions live in App/Views, owned by the matching worker) --------
    LightingView lighting;
    MidiView midi;
    MediaView media;
    RecordView record;
    BroadcastView broadcast;
    // --- Diagnostics tab ---------------------------------------------------------------------------
    std::vector<ThreadView> threads;
    std::vector<AdapterRow> adapters;
    std::vector<DiagEvent> recentDiag;
    std::uint64_t actionsPending = 0, actionsDropped = 0, diagDropped = 0;
    std::vector<std::string> startupSteps;    // "start.audio 1.2 ms done|stubbed|failed", in order
};

// ---- action results and commands -----------------------------------------------------------------
struct ActionOutcome {
    std::uint64_t correlationId = 0;
    bool accepted = false;
    std::string reason;                       // sanitised text from core, shown by the UI on rejection
    ss::Revision revision = 0;
    std::uint32_t actionId = 0;
};

// Requests that are not part of the action registry (core/base is append-only and not ours to extend).
// Append-only numbering. Each kind has exactly one owner that registers its handler on the controller.
enum class AppCommandKind : std::uint16_t {
    None = 0,
    SelectAudioDevice = 1,      // text = type, text2 = device name           owner: engine-adapters-desktop
    SetAudioRate = 2,           // a = rate Hz                                 owner: engine-adapters-desktop
    SelectLightingInterface = 3,// text = ipv4                                 owner: engine-adapters-desktop
    SetLightingDestination = 4, // text = ip, a = port                         owner: engine-adapters-desktop
    OpenMidiPort = 5,           // text = identity, a = 1 input / 0 output     owner: engine-adapters-desktop
    CloseMidiPort = 6,
    SetFfmpegPath = 10,         // text = path                                 owner: engine-media-record
    SetRecordFolder = 11,       // text = folder                               owner: engine-media-record
    OpenCamera = 12,            // text = camera id                            owner: engine-media-record
    CloseCamera = 13,
    LoadShowFile = 20,          // text = path (already validated by the dialog callback)   owner: shell-integration
    SaveShowFile = 21,          // text = path, empty = current                              owner: shell-integration
    ExportShowReport = 22,      // text = path                                 owner: ui-other
    ExportSupportBundle = 23,   // text = path                                 owner: ui-other
    RunShowDoctor = 24,         //                                             owner: ui-other
    RecoveryRestoreAutosave = 25,// Recovery banner: restore the autosave (writes a restored copy)   owner: shell-integration
    RecoveryOpenLast = 26,      // Recovery banner: open the last saved file                          owner: shell-integration
    RecoveryStartEmpty = 27,    // Recovery banner: start with the built-in sample show                owner: shell-integration
    SetPref = 28,               // text = key ("ui.*", "display.*", "wizard.*"), text2 = value        owner: shell-integration
    NewShow = 29,               // sample show, unsaved                                                owner: shell-integration
    ArmLighting = 30,           // explicit operator Arm; never issued by restore/recovery  owner: engine-adapters-desktop
    DisarmLighting = 31,
    EnableMidiOutput = 32,
    DisableMidiOutput = 33,
    // 40 and up: integration commands added after the UI workers finished (append-only).
    SetBroadcastEndpoint = 40,  // text = host, text2 = path, a = port                                 owner: shell-integration
    SetBroadcastKey = 41,       // text = stream key; stored in the secret store, never echoed         owner: shell-integration
    MidiLearnBegin = 42,        //                                                                      owner: shell-integration
    MidiLearnCancel = 43,
    MidiLearnCommit = 44,       // a = ActionId, b = EncoderMode (0 absolute)
    MidiRemoveMapping = 45,     // a = mapping id
    SetLightingBroadcast = 46,  // a = 1 on / 0 off (Art-Net directed broadcast opt-in)
    PublishText = 47,           // text = message; handed to the media worker when it can raster text
    ImportMedia = 48,           // text = file path; handed to the media worker
    RemoveMedia = 49,           // a = image slot 0..2
    ConfigureMedia = 50,        // text = ffmpeg path (empty = search), text2 = recordings folder (empty = default), a = program output (0 = keep), b = video on 1 / off 0
    PublishCamera = 51,        // a = logical output; explicit operator publication, never restore
    UnpublishCamera = 52,
    RefreshCameras = 53,
    Count_ = 54
};

struct AppCommand {
    AppCommandKind kind = AppCommandKind::None;
    std::string text, text2;
    std::int64_t a = 0, b = 0;
    double value = 0.0;
    std::uint64_t correlationId = 0;
};

struct CommandOutcome {
    std::uint64_t correlationId = 0;
    AppCommandKind kind = AppCommandKind::None;
    bool ok = false;
    std::string message;                      // sanitised
};

// ---- listener (message thread) --------------------------------------------------------------------
class UiModelListener {
public:
    virtual ~UiModelListener() = default;
    virtual void uiSnapshotChanged(const UiSnapshot&) {}             // at most once per poll when version moved
    virtual void uiActionOutcome(const ActionOutcome&) {}
    virtual void uiCommandOutcome(const CommandOutcome&) {}
};

// ---- the model -------------------------------------------------------------------------------------
class UiModel {
public:
    static constexpr std::size_t kCommandQueue = 64, kOutcomeQueue = 256;
    UiModel();

    // ===== for UI code =====================================================================================
    // Any thread. Assigns correlationId when it is 0. Never blocks. Safety-critical actions bypass the queue.
    struct Ticket { SubmitResult result; std::uint64_t correlationId = 0; };
    Ticket submitAction(ss::Action a);
    // Any thread. Returns false when the queue is full. Handled on the IO thread (blocking allowed there).
    bool postCommand(AppCommand c, std::uint64_t* correlationOut = nullptr);

    std::shared_ptr<const UiSnapshot> snapshot() const { return snap_.get(); }   // never null after the first publish
    std::uint64_t version() const { return snap_.version(); }
    std::shared_ptr<const SharedFrame> latestFrame(OutputId id) const;           // preview thumbnails etc.

    // Message thread only. Call from a 30 Hz timer; notifies listeners when something changed.
    void addListener(UiModelListener* l);
    void removeListener(UiModelListener* l);
    void poll();
    void setPollHook(std::function<void()> f) { pollHook_ = std::move(f); }   // message thread, runs each poll

    // ===== for the controller (not for UI) =====================================================================
    ActionInbox& inbox() { return inbox_; }
    void publish(std::shared_ptr<const UiSnapshot> s) { snap_.publish(std::move(s)); }
    void postOutcome(const ActionOutcome& o) { outcomes_.push(o); }
    void postCommandOutcome(const CommandOutcome& o);
    bool popCommand(AppCommand& out);                                   // IO thread only
    void setFrameSource(std::function<std::shared_ptr<const SharedFrame>(OutputId)> f) { frameSource_ = std::move(f); }

private:
    ActionInbox inbox_;
    LatestShared<UiSnapshot> snap_;
    ss::SpscQueue<ActionOutcome> outcomes_;
    std::mutex cmdMutex_, cmdOutMutex_;
    std::deque<AppCommand> commands_;
    std::deque<CommandOutcome> commandOutcomes_;
    std::atomic<std::uint64_t> nextCorrelation_{1};
    std::function<std::shared_ptr<const SharedFrame>(OutputId)> frameSource_;
    std::vector<UiModelListener*> listeners_;      // message thread only
    std::uint64_t notifiedVersion_ = 0;
    std::function<void()> pollHook_;
};

}  // namespace ss::app
