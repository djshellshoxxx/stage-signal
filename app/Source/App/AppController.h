// Contract: the object graph, thread table and lifecycle of the app (app-shell sections 3, 4, 5, 9).
// Owner: shell-integration. One AppController per process owns every thread, adapter and core object.
// JUCE-free header; the .cpp uses juce::Thread for the workers.
//
//   start(): app-shell section 5 startup order, steps 1..14. Nothing transmits, records, broadcasts or
//            publishes camera. Lighting and MIDI-OUT threads exist but are DISARMED.
//   stop():  app-shell section 9 shutdown stages 1..8, each timed; a stuck stage is logged and skipped.
//
// Thread table (section 4.1) as implemented here:
//   CONTROL  drains the ActionInbox into the show engine (single writer), ticks the show at 25 Hz, publishes the
//            ShowSnapshot (full + lite), builds the UiSnapshot at 30 Hz, samples perf counters at 1 Hz.
//   ANALYSIS pulls AudioIngress into audio + dynamics; publishes FeatureSnapshot; feeds processed-audio consumers.
//   RENDER   renders via render/compose cores into one FrameExchange per output.
//   LIGHT    LightingScheduler ticks (disarmed by default).      MIDI-OUT  MidiScheduler ticks (off by default).
//   IO       drains DiagBus into the journal/log, runs AppCommand handlers (blocking work allowed here).
//   MEDIA / CAMERA / MIDI-IN / AUDIO are adapter-owned threads (see Engine/I*.h).
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "App/AnalysisWorker.h"
#include "App/AudioIngress.h"
#include "App/DiagBus.h"
#include "App/HostMode.h"
#include "App/LightingScheduler.h"
#include "App/MediaWorker.h"
#include "App/MidiScheduler.h"
#include "App/RenderHost.h"
#include "App/UiModel.h"
#include "Engine/AdapterSet.h"
#include "ss/diag/journal.hpp"
#include "ss/show/engine.hpp"

namespace ss::app {

struct AppConfig {
    HostKind host = HostKind::Standalone;
    bool headless = false;            // no UI will attach (selftest, CI); only recorded in the snapshot
    bool recovery = false;            // Recovery mode: idle scene, nothing armed (also set by a stale running.lock)
    bool startAudio = true;           // standalone: open the audio source at step 9 (CLAP never does)
    bool useSampleShow = true;        // step 4 fallback
    double renderFps = 25.0;
    // ---- persistence (empty dataDir = nothing is read or written: self-tests, CI) --------------------------------
    std::string dataDir;              // settings.json, prefs.txt, running.lock, shows/ live here
    std::string showPath;             // --show <file>: open this show instead of the last one
    bool safeMode = false;            // --safe: ignore the last show and saved devices, start from defaults
    bool secondary = false;           // plugin Secondary instance: analysis and UI only, every output refused
    // ---- shutdown test seams -------------------------------------------------------------------------------------
    int stage2TimeoutMs = 5000;       // finalise-writers stage budget
    std::function<void(const std::atomic<bool>& childrenKilled)> stage2Writer;   // runs in stage 2 (tests inject a stuck writer)
};

enum class StepStatus : unsigned char { Done, Stubbed, Failed };

struct StepRecord {
    std::string name;                 // "start.audio", "shutdown.stage2", ...
    double ms = 0.0;
    StepStatus status = StepStatus::Done;
    bool timedOut = false;            // shutdown stages only
    std::string detail;
};

class AppController {
public:
    using ActionHandler = std::function<ss::ActionResult(const ss::Action&)>;   // CONTROL thread, must be quick
    using CommandHandler = std::function<ss::Status(const AppCommand&)>;        // IO thread, may block

    AppController(AdapterSet adapters, AppConfig cfg = {});
    ~AppController();                 // calls stop()
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // ---- lifecycle (message thread) ---------------------------------------------------------------------
    ss::Status start();               // idempotent
    void stop();                      // idempotent
    bool started() const { return started_; }
    std::vector<StepRecord> startupLog() const { return startLog_; }
    std::vector<StepRecord> shutdownLog() const { return stopLog_; }

    // ---- wiring (before start()) ---------------------------------------------------------------------------
    void addAudioConsumer(IProcessedAudioConsumer* c) { analysis_.addConsumer(c); }
    void registerActionHandler(ss::ActionId id, ActionHandler h);          // actions the show engine does not own
    void registerCommandHandler(AppCommandKind k, CommandHandler h);

    // ---- access ------------------------------------------------------------------------------------------------
    UiModel& ui() { return ui_; }
    AudioIngress& ingress() { return ingress_; }          // StageSignalProcessor holds a non-owning pointer to this
    const HostMode& hostMode() const { return hostModeFor(cfg_.host); }
    AdapterSet& adapters() { return adapters_; }
    const ss::IClock& clock() const { return *adapters_.clock; }
    LightingScheduler& lighting() { return lighting_; }
    MidiScheduler& midi() { return midi_; }
    MediaWorker& media() { return media_; }
    AnalysisWorker& analysis() { return analysis_; }
    RenderHost& renderer() { return render_; }
    Heartbeats& heartbeats() { return hb_; }
    DiagBus& diag() { return diag_; }

    // Any thread. Validates fully; the old show stays on failure. Also republishes the scene table and outputs.
    ss::Status loadShow(const ss::model::ShowDocument& doc);

    // ---- shell services (AppShell.cpp) -------------------------------------------------------------------------
    bool isSecondary() const { return cfg_.secondary; }
    bool inRecovery() const { return recovery_.load(); }
    std::string pref(const std::string& key, const std::string& fallback = std::string()) const;
    // Versioned, bounded plugin state (app-shell 7.3). Never carries armed/recording flags. Any thread.
    ss::Result<std::string> getStateBlob() const;
    ss::Status setStateBlob(const void* data, std::size_t size);      // stages, validates, then loads at a safe point
    void setPreference(const std::string& key, const std::string& value);   // validated; saved to prefs.txt
    MusicView musicView() const;                                      // last phrase / key colour / beat-look state
    void driveMusic(const ss::show::ShowSnapshot& snap, const ss::FeatureSnapshot& f, ss::Ns now) { shellMusicTick(snap, f, now); }   // CONTROL does this itself; tests call it on a controller that is not started
    bool hasUnsavedWork() const;                                      // show differs from the saved file
    bool mediaActive() const;                                         // recording or broadcast running (quit confirm)
    ss::Status saveShowNow();                                         // IO-thread semantics, blocking; tests and quit path
    void discardUnsavedWork();                                        // the operator chose "discard": drops recovery files
    bool latestFeatures(ss::FeatureSnapshot& out) const { return analysis_.features(out); }

private:
    class Loop;                       // juce::Thread wrapper, defined in the .cpp
    ss::Status runStartStep(const char* name, bool stubbed, const std::function<ss::Status()>& fn, bool fatal);
    void runStopStage(const char* name, int timeoutMs, const std::function<bool()>& fn);

    void controlBody(Loop&);
    void analysisBody(Loop&);
    void renderBody(Loop&);
    void lightBody(Loop&);
    void midiBody(Loop&);
    void ioBody(Loop&);
    // shell internals
    struct Shell;
    ss::Status shellLoadSettings();
    ss::Status shellCrashMarker();
    ss::Status shellLoadShow();
    void shellWriteLock();
    void shellRemoveLock();
    void shellInstallWiring();                                        // MIDI action sink, command handlers, MIDI/lighting config
    void shellConfigureEngines(const ss::model::ShowDocument& doc);
    void shellIoTick(ss::Ns now);                                     // autosave, device refresh, learn poll (IO thread)
    void shellFillUi(UiSnapshot& s, ss::Ns now);                      // CONTROL thread
    void shellSavePrefs();
    void shellSaveSettings();
    ss::Status shellOpenPath(const std::string& path);
    ss::Status shellSavePath(const std::string& path);
    void shellRegisterCommands();
    void controlAdapt(ss::Ns now);
    void shellMessageTick();
    void shellMusicTick(const ss::show::ShowSnapshot& snap, const ss::FeatureSnapshot& f, ss::Ns now);
    void drainActions();
    ss::ActionResult dispatch(const ss::Action& a);
    void showTick(ss::Ns now);
    void publishUi(ss::Ns now);
    void executeCommand(const AppCommand& c);
    void startLoop(std::unique_ptr<Loop>& slot, const char* name, ThreadId id, bool highPriority,
                   void (AppController::*body)(Loop&));
    bool stopLoop(std::unique_ptr<Loop>& slot, int timeoutMs);

    AdapterSet adapters_;
    AppConfig cfg_;
    bool started_ = false;
    std::vector<StepRecord> startLog_, stopLog_;

    // graph (declaration order = construction order; destruction is reverse and stop() has already run)
    DiagBus diag_;
    ss::diag::EventJournal journal_;                   // IO thread is the single writer
    Heartbeats hb_;
    UiModel ui_;
    AudioIngress ingress_;
    ss::show::ShowEngine show_;
    LatestShared<ss::show::ShowSnapshot> showSnap_;
    ss::LatestMailbox<ss::ShowSnapshotLite> showLite_;
    LatestShared<SceneTable> sceneTable_;
    AnalysisWorker analysis_;
    RenderHost render_;
    LightingScheduler lighting_;
    MidiScheduler midi_;
    MediaWorker media_;
    std::unordered_map<std::uint32_t, ActionHandler> actionHandlers_;
    std::unordered_map<std::uint16_t, CommandHandler> commandHandlers_;
    PerfView perf_;                                    // CONTROL thread only
    std::unique_ptr<Shell> shell_;
    std::atomic<bool> recovery_{false};
    std::atomic<bool> childrenKilled_{false};
    std::thread stage2Worker_;
    std::uint64_t onsetCursor_ = 0;                    // CONTROL thread: analysis onset drain
    std::unique_ptr<Loop> control_, analysisT_, renderT_, light_, midiT_, io_;
};

}  // namespace ss::app
