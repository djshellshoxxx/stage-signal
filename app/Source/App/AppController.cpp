#include "App/AppController.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <juce_core/juce_core.h>
#include "App/SampleShow.h"
#include "App/ShellImpl.h"
#include "Engine/Fakes.h"

namespace ss::app {

namespace {

AdapterSet withDefaults(AdapterSet s) {
    AdapterSet d = makeFakeAdapterSet(false);
    if (!s.clock) s.clock = d.clock;
    if (!s.audio) s.audio = d.audio;
    if (!s.timestampMapper) s.timestampMapper = d.timestampMapper;
    if (!s.midi) s.midi = d.midi;
    if (!s.packets) s.packets = d.packets;
    if (!s.serial) s.serial = d.serial;
    if (!s.displays) s.displays = d.displays;
    if (!s.camera) s.camera = d.camera;
    if (!s.processes) s.processes = d.processes;
    if (!s.secrets) s.secrets = d.secrets;
    if (!s.files) s.files = d.files;
    if (!s.zip) s.zip = d.zip;
    if (!s.perf) s.perf = d.perf;
    if (!s.dialogs) s.dialogs = d.dialogs;
    if (!s.log) s.log = d.log;
    return s;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// ---- worker thread wrapper ------------------------------------------------------------------------------
class AppController::Loop final : public juce::Thread {
public:
    Loop(const char* name, ThreadId id, Heartbeats& hb, std::function<void(Loop&)> body, std::function<void(const std::string&)> onError)
        : juce::Thread(name), id_(id), hb_(hb), body_(std::move(body)), onError_(std::move(onError)) {}
    void run() override {
        hb_.markRunning(id_, true);
        // juce::Thread swallows exceptions and ends the thread silently. A worker must not vanish: report it and run again.
        while (!threadShouldExit()) {
            try { body_(*this); break; }
            catch (const std::exception& e) { if (onError_) onError_(std::string(getThreadName().toRawUTF8()) + " thread error: " + e.what()); }
            catch (...) { if (onError_) onError_(std::string(getThreadName().toRawUTF8()) + " thread error"); }
            wait(50);
        }
        hb_.markRunning(id_, false);
    }
private:
    std::function<void(const std::string&)> onError_;
    ThreadId id_;
    Heartbeats& hb_;
    std::function<void(Loop&)> body_;
};

// ---- construction ----------------------------------------------------------------------------------------
AppController::AppController(AdapterSet adapters, AppConfig cfg)
    : adapters_(withDefaults(std::move(adapters))),
      cfg_(cfg),
      diag_(*adapters_.clock),
      journal_(*adapters_.clock, 4096),
      ingress_(*adapters_.timestampMapper),
      show_(adapters_.clock.get()),
      analysis_(ingress_, *adapters_.clock),
      render_(*adapters_.clock, ui_.inbox().latch(), showSnap_, sceneTable_, analysis_.featureMailbox(), analysis_.visualMailbox()),
      lighting_(*adapters_.clock, *adapters_.packets, ui_.inbox().latch(), showLite_, analysis_.featureMailbox()),
      midi_(*adapters_.clock, *adapters_.midi),
      media_(adapters_) {
    shell_ = std::make_unique<Shell>();
    recovery_.store(cfg_.recovery);
    render_.setTargetFps(cfg_.renderFps);
    render_.setLayerSource(&media_);
    media_.setFrameProvider([this](OutputId id) { return render_.latestFrame(id); });
    analysis_.addConsumer(&media_);
    ui_.setPollHook([this] { shellMessageTick(); });
    ui_.setFrameSource([this](OutputId id) { return render_.latestFrame(id); });

    // Built-in routing for actions the show engine does not own (app-shell 4.3).
    registerActionHandler(ss::ActionId::ArmLighting, [this](const ss::Action& a) {
        ss::ActionResult r;
        if (cfg_.secondary) { r.reason = "Another StageSignal in this host owns the outputs"; return r; }
        // Arm is an explicit operator act: never from Show/System/Remote sources (REQ-L05).
        if (a.source != ss::ActionSource::Ui && a.source != ss::ActionSource::Keyboard && a.source != ss::ActionSource::Midi) {
            r.reason = "Arm needs an operator action";
            return r;
        }
        auto s = lighting_.arm();
        r.accepted = s.ok();
        if (!s.ok()) r.reason = s.error().detail;
        return r;
    });
    registerActionHandler(ss::ActionId::DisarmLighting, [this](const ss::Action&) {
        lighting_.disarm();
        ss::ActionResult r; r.accepted = true; return r;
    });
    registerActionHandler(ss::ActionId::LightingDryRun, [](const ss::Action&) {
        ss::ActionResult r; r.reason = "Lighting dry run is not available in this build"; return r;
    });
    for (auto id : {ss::ActionId::StartRecording, ss::ActionId::StopRecording, ss::ActionId::StartBroadcast,
                    ss::ActionId::StopBroadcast})
        registerActionHandler(id, [this](const ss::Action& a) {
            if (cfg_.secondary) { ss::ActionResult r; r.reason = "Another StageSignal in this host owns the outputs"; return r; }
            return media_.handleAction(a);
        });
    registerActionHandler(ss::ActionId::AddMarker, [this](const ss::Action& a) {
        diag_.post(DiagLevel::Info, "marker", "Marker");        // journal marker; the recorder also writes one when recording
        media_.handleAction(a);
        ss::ActionResult r; r.accepted = true; return r;
    });

    registerCommandHandler(AppCommandKind::ArmLighting, [this](const AppCommand&) { return lighting_.arm(); });
    registerCommandHandler(AppCommandKind::DisarmLighting, [this](const AppCommand&) { lighting_.disarm(); return ss::okStatus(); });
    registerCommandHandler(AppCommandKind::EnableMidiOutput, [this](const AppCommand&) { midi_.enableOutput(); return ss::okStatus(); });
    registerCommandHandler(AppCommandKind::DisableMidiOutput, [this](const AppCommand&) { midi_.disableOutput(); return ss::okStatus(); });
    shellInstallWiring();
}

AppController::~AppController() { stop(); }

void AppController::registerActionHandler(ss::ActionId id, ActionHandler h) { actionHandlers_[ss::id(id)] = std::move(h); }
void AppController::registerCommandHandler(AppCommandKind k, CommandHandler h) {
    commandHandlers_[static_cast<std::uint16_t>(k)] = std::move(h);
}

// ---- show loading ----------------------------------------------------------------------------------------
ss::Status AppController::loadShow(const ss::model::ShowDocument& doc) {
    auto st = show_.loadDocument(doc);
    if (!st.ok()) return st;
    auto table = std::make_shared<SceneTable>();
    for (const auto& sc : doc.scenes) table->scenes[sc.id] = sc;
    table->outputs = doc.outputs;
    sceneTable_.publish(table);
    std::vector<OutputSpec> specs;
    for (const auto& o : doc.outputs) specs.push_back({o.id, static_cast<std::uint32_t>(o.width), static_cast<std::uint32_t>(o.height), 0});
    render_.setOutputs(std::move(specs));
    shellConfigureEngines(doc);
    return ss::okStatus();
}

// ---- lifecycle -------------------------------------------------------------------------------------------
ss::Status AppController::runStartStep(const char* name, bool stubbed, const std::function<ss::Status()>& fn, bool fatal) {
    const auto t0 = std::chrono::steady_clock::now();
    ss::Status st = fn();
    StepRecord r;
    r.name = std::string("start.") + name;
    r.ms = msSince(t0);
    r.status = !st.ok() ? StepStatus::Failed : (stubbed ? StepStatus::Stubbed : StepStatus::Done);
    if (!st.ok()) r.detail = st.error().detail;
    { std::lock_guard<std::mutex> g(logMutex_); startLog_.push_back(r); }
    diag_.post(st.ok() ? DiagLevel::Info : DiagLevel::Error, r.name, r.detail.empty() ? (stubbed ? "stubbed" : "ok") : r.detail);
    if (!st.ok() && !fatal) return ss::okStatus();     // subsystem Unavailable, startup continues (section 5)
    return st;
}

void AppController::startLoop(std::unique_ptr<Loop>& slot, const char* name, ThreadId id, bool highPriority,
                              void (AppController::*body)(Loop&)) {
    slot = std::make_unique<Loop>(name, id, hb_, [this, body](Loop& l) { (this->*body)(l); },
                                  [this](const std::string& what) { diag_.post(DiagLevel::Error, "thread", what); std::fprintf(stderr, "%s\n", what.c_str()); });
    slot->startThread(highPriority ? juce::Thread::Priority::high : juce::Thread::Priority::normal);
}

bool AppController::stopLoop(std::unique_ptr<Loop>& slot, int timeoutMs) {
    if (!slot) return true;
    const bool clean = slot->stopThread(timeoutMs);
    slot.reset();
    return clean;
}

ss::Status AppController::start() {
    if (started_) return ss::okStatus();
    { std::lock_guard<std::mutex> g(logMutex_); startLog_.clear(); }
    ss::Status st = ss::okStatus();
    auto step = [&](const char* name, bool stubbed, std::function<ss::Status()> fn, bool fatal = false) {
        if (!st.ok()) return;
        st = runStartStep(name, stubbed, fn, fatal);
    };
    // 1 Diagnostics bus + log files. The IO thread drains the bus, so it starts first.
    step("diag", false, [&] { startLoop(io_, "IO", ThreadId::Io, false, &AppController::ioBody); return ss::okStatus(); }, true);
    // 2 Settings and secret store open (settings persistence itself is shell-integration follow-up work).
    step("settings", false, [&] {
        auto st = adapters_.secrets->start();
        auto ls = shellLoadSettings();
        return st.ok() ? ls : st;
    });
    // 3 Crash marker check: running.lock handling is a follow-up; cfg_.recovery is honoured by step 4/5.
    step("crashmarker", shell_->dir.empty(), [&] { return shellCrashMarker(); });
    // 4 Show document: built-in sample show (persist-backed load of the last show is a follow-up).
    step("show", false, [&] { auto r = shellLoadShow(); shell_->ready.store(true); return r; });
    // 5 Core engines: show engine exists since construction; router/palette/perf are not wired yet. CONTROL starts.
    step("engines", false, [&] { startLoop(control_, "CONTROL", ThreadId::Control, true, &AppController::controlBody); return ss::okStatus(); }, true);
    // 6 Renderer.
    step("renderer", false, [&] { startLoop(renderT_, "RENDER", ThreadId::Render, false, &AppController::renderBody); return ss::okStatus(); });
    // 7 UI and audience windows are created by the host application / editor, not by the controller.
    step("ui", true, [] { return ss::okStatus(); });
    // 8 Analysis worker, prepared before audio starts so the first block has a consumer.
    step("analysis", false, [&] { startLoop(analysisT_, "ANALYSIS", ThreadId::Analysis, true, &AppController::analysisBody); return ss::okStatus(); });
    // 9 Audio. Standalone opens the saved device (never another one); CLAP waits for the host's prepareToPlay.
    step("audio", !hostMode().ownsAudioDevice(), [&]() -> ss::Status {
        adapters_.audio->setSink(&ingress_);
        if (hostMode().ownsAudioDevice() && !shell_->audioName.empty()) {       // the saved device, never another one
            AudioDeviceDesc d; d.typeName = shell_->audioType; d.deviceName = shell_->audioName;
            adapters_.audio->selectDevice(d);
        }
        if (!hostMode().ownsAudioDevice() || !cfg_.startAudio) return ss::okStatus();
        auto s = adapters_.audio->start();
        if (!s.ok()) diag_.post(DiagLevel::Warn, "audio", "No audio device");
        return ss::okStatus();         // "No audio device" is a state, not a startup failure (REQ-A02)
    });
    // 10 MIDI input: provider started, no port opened until mappings ask for one.
    step("midi", false, [&] {
        auto st = adapters_.midi->start();
        for (const auto& id : shell_->wantedInputs) midi_.driver().wantInput(id);   // saved devices by stable identity
        return st;
    });
    // 11 Schedulers constructed DISARMED: LIGHT and MIDI-OUT threads run, but transmit nothing.
    step("schedulers", false, [&] {
        lighting_.disarm(); midi_.disableOutput();
        startLoop(light_, "LIGHT", ThreadId::Light, true, &AppController::lightBody);
        startLoop(midiT_, "MIDI-OUT", ThreadId::MidiOut, true, &AppController::midiBody);
        return ss::okStatus();
    });
    // 12 Camera and media workers: idle, camera closed, publication off.
    step("media", false, [&] { shellPushMediaConfig(); adapters_.camera->start(); return media_.start(); });
    // 13 Perf monitor: 1 Hz counter sampling runs inside the CONTROL loop. No benchmark runs automatically.
    step("perf", false, [] { return ss::okStatus(); });
    // 14 Ready. running.lock is not written yet (follow-up); stated, not faked.
    step("ready", false, [&] { shellWriteLock(); return ss::okStatus(); });
    if (!st.ok()) { stop(); return st; }
    started_ = true;
    diag_.post(DiagLevel::Info, "startup.complete", "ready");
    return ss::okStatus();
}

void AppController::runStopStage(const char* name, int timeoutMs, const std::function<bool()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool clean = fn();
    StepRecord r;
    r.name = std::string("shutdown.") + name;
    r.ms = msSince(t0);
    r.timedOut = !clean || r.ms > timeoutMs;
    r.status = StepStatus::Done;
    if (r.timedOut) r.detail = "timeout";
    { std::lock_guard<std::mutex> g(logMutex_); stopLog_.push_back(r); }
    diag_.post(r.timedOut ? DiagLevel::Warn : DiagLevel::Info, r.name + (r.timedOut ? ".timeout" : ""), "");
}

void AppController::stop() {
    const bool anyThread = control_ || analysisT_ || renderT_ || light_ || midiT_ || io_;
    if (!started_ && !anyThread) return;
    { std::lock_guard<std::mutex> g(logMutex_); stopLog_.clear(); }
    runStopStage("stage1.disarm", 200, [&] {              // 1 disarm outward actions
        lighting_.disarm(); midi_.disableOutput(); media_.unpublishCamera(); return true;
    });
    childrenKilled_.store(false);
    runStopStage("stage2.finalise", cfg_.stage2TimeoutMs, [&] {                       // 2 cancel broadcast, finalise writers
        // Writers can block on a stuck child process. Run them on a helper and wait up to the budget; stage 6 kills the
        // children, which releases a stuck writer, and the helper is joined at the very end of stop().
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        stage2Worker_ = std::thread([this, done] {
            if (cfg_.stage2Writer) cfg_.stage2Writer(childrenKilled_);
            media_.stop();
            { std::lock_guard<std::mutex> g(shell_->m); if (shell_->session && !shell_->dirty) shell_->session->closeClean(); }
            done->set_value();
        });
        return fut.wait_for(std::chrono::milliseconds(cfg_.stage2TimeoutMs)) == std::future_status::ready;
    });
    runStopStage("stage3.quiesce", 1000, [&] {            // 3 quiesce MIDI and lighting
        bool ok = stopLoop(light_, 1000);
        ok = stopLoop(midiT_, 1000) && ok;
        return ok;
    });
    runStopStage("stage4.capture", 1000, [&] {            // 4 stop capture and camera
        if (hostMode().ownsAudioDevice()) adapters_.audio->stop();
        adapters_.audio->setSink(nullptr);
        adapters_.camera->close();
        return true;
    });
    runStopStage("stage5.drain", 6000, [&] {              // 5 drain bounded tasks
        bool ok = stopLoop(analysisT_, 2000);
        ok = stopLoop(control_, 2000) && ok;
        ok = stopLoop(io_, 2000) && ok;
        std::vector<DiagEvent> ev;                        // final flush; the IO thread (single journal writer) is gone
        diag_.drain(ev);
        for (auto& e : ev) journal_.append(e.level == DiagLevel::Error ? ss::diag::EventKind::Error : ss::diag::EventKind::Note, e.tag + " " + e.text);
        return ok;
    });
    runStopStage("stage6.workers", 1000, [&] {            // 6 destroy worker resources
        adapters_.midi->stop(); adapters_.packets->stop(); adapters_.serial->stop();
        adapters_.camera->stop(); adapters_.processes->stop(); adapters_.secrets->stop();
        childrenKilled_.store(true);                      // a stuck writer waiting on a child is released here
        return true;
    });
    runStopStage("stage7.renderer", 500, [&] { return stopLoop(renderT_, 500); });   // 7 destroy renderer
    runStopStage("stage8.ui", 500, [&] {                  // 8 close UI, remove running.lock
        if (stage2Worker_.joinable()) stage2Worker_.join();
        shellSaveSettings(); shellSavePrefs();
        shellRemoveLock();
        return true;
    });
    started_ = false;
}

// ---- thread bodies ---------------------------------------------------------------------------------------
void AppController::drainActions() {
    ss::Action a;
    for (int budget = 256; budget > 0 && ui_.inbox().pop(a); --budget) {
        ss::ActionResult r = dispatch(a);
        ActionOutcome o;
        o.correlationId = a.correlationId; o.accepted = r.accepted; o.reason = r.reason; o.revision = r.revision;
        o.actionId = a.actionId;
        ui_.postOutcome(o);
        if (a.actionId == ss::id(ss::ActionId::RestoreBlackout) && r.accepted) ui_.inbox().latch().clearAll();
    }
}

ss::ActionResult AppController::dispatch(const ss::Action& a) {
    auto it = actionHandlers_.find(a.actionId);
    if (it != actionHandlers_.end()) return it->second(a);
    return show_.submit(a);
}

void AppController::showTick(ss::Ns now) {
    ss::FeatureSnapshot f;
    if (!analysis_.signalAlive(now) || !analysis_.features(f)) f = ss::FeatureSnapshot{};   // silent, never stale
    // Every onset since the last tick counts, even when the mailbox's edge latch was overwritten in between.
    if (analysis_.onsetsSince(onsetCursor_) > 0) f.onsetFired = true;
    auto snap = std::make_shared<ss::show::ShowSnapshot>(show_.tick(now, f));
    if (recoveryPausePending_.load()) {                  // Recovery holds the show: repeat until the snapshot says paused
        if (snap->paused || !recovery_.load()) recoveryPausePending_.store(false);
        else { ss::Action p; p.actionId = ss::id(ss::ActionId::Pause); p.source = ss::ActionSource::System; show_.submit(p); }
    }
    shellMusicTick(*snap, f, now);
    { std::lock_guard<std::mutex> g(shell_->m); shell_->history.offer(now, f, snap->outputs.empty() ? 0 : snap->outputs.front().sceneId); }
    for (const auto& ev : snap->events) {
        if (ev.kind == ss::show::EventKind::SceneChanged) diag_.post(DiagLevel::Info, "scene", std::to_string(ev.sceneId) + " " + ev.detail);
        else if (ev.kind == ss::show::EventKind::ModeChanged) diag_.post(DiagLevel::Info, "mode", ev.detail);
    }
    showLite_.publish(snap->lite());
    showSnap_.publish(std::move(snap));
}

void AppController::publishUi(ss::Ns now) {
    auto s = std::make_shared<UiSnapshot>();
    s->version = ui_.version() + 1;
    s->atNs = now;
    s->host = cfg_.host; s->headless = cfg_.headless;
    analysis_.features(s->features);
    analysis_.visual(s->visual);
    if (auto sh = showSnap_.get()) s->show = *sh;
    const AudioIngressStats ist = ingress_.stats();
    const AdapterHealth ah = adapters_.audio->health();
    s->audio.deviceState = ah.state; s->audio.deviceError = ah.lastError;
    s->audio.signalAlive = analysis_.signalAlive(now);
    s->audio.rateHz = ist.rateHz; s->audio.channels = ist.channels;
    s->audio.blocks = ist.blocks; s->audio.droppedBlocks = ist.droppedBlocks;
    s->audio.sanitisedSamples = ist.sanitisedSamples; s->audio.discontinuities = ist.discontinuities;
    s->audio.dynamics = analysis_.dynamicsTelemetry();
    const RenderStats rs = render_.stats();
    s->perf = perf_;
    s->perf.renderFps = rs.fpsMeasured; s->perf.renderMsLast = rs.lastFrameMs; s->perf.renderMsWorst = rs.worstFrameMs;
    if (auto table = sceneTable_.get()) {
        for (const auto& o : table->outputs) {
            OutputView ov;
            ov.id = o.id; ov.label = o.label; ov.kind = o.kind; ov.width = o.width; ov.height = o.height;
            ov.framesRendered = rs.framesRendered; ov.framesDropped = rs.framesDropped;
            ov.blackout = s->show.visualBlackout || ui_.inbox().latch().anyVisual();
            s->outputs.push_back(std::move(ov));
        }
    }
    lighting_.fillView(s->lighting);
    midi_.fillView(s->midi);
    media_.fillView(s->media, s->record, s->broadcast);
    for (int i = 0; i < static_cast<int>(ThreadId::Count); ++i) {
        const auto t = static_cast<ThreadId>(i);
        s->threads.push_back({threadName(t), hb_.running(t), hb_.stale(t, now), hb_.beats(t)});
    }
    auto row = [&](const char* n, const IAdapter* a) {
        if (!a) return;
        const AdapterHealth h = a->health();
        s->adapters.push_back({n, h.state, h.lastError});
    };
    row("audio", adapters_.audio.get()); row("midi", adapters_.midi.get()); row("lighting transport", adapters_.packets.get());
    row("serial dmx", adapters_.serial.get()); row("displays", adapters_.displays.get()); row("camera", adapters_.camera.get());
    row("processes", adapters_.processes.get()); row("secrets", adapters_.secrets.get());
    s->recentDiag = diag_.recent();
    s->actionsPending = ui_.inbox().pending(); s->actionsDropped = ui_.inbox().dropped(); s->diagDropped = diag_.dropped();
    for (const auto& r : startupLog()) {
        const char* tag = r.status == StepStatus::Failed ? "failed" : (r.status == StepStatus::Stubbed ? "stubbed" : "done");
        s->startupSteps.push_back(r.name + " " + std::to_string(r.ms) + " ms " + tag);
    }
    shellFillUi(*s, now);
    ui_.publish(std::move(s));
}

void AppController::controlBody(Loop& L) {
    ss::Ns nextShow = 0, nextUi = 0, nextPerf = 0;
    while (!L.threadShouldExit()) {
        const ss::Ns now = clock().nowMonotonic();
        drainActions();
        controlAdapt(now);
        if (now >= nextShow) { showTick(now); nextShow = now + 40 * ss::kNsPerMs; }               // 25 Hz
        if (now >= nextPerf) {                                                                       // 1 Hz
            const PerfSample p = adapters_.perf->sample();
            perf_.valid = p.valid; perf_.cpuPct = p.processCpuPct; perf_.workingSetBytes = p.workingSetBytes;
            nextPerf = now + ss::kNsPerSec;
        }
        if (now >= nextUi) { publishUi(now); nextUi = now + 33 * ss::kNsPerMs; }                    // ~30 Hz
        hb_.beat(ThreadId::Control, clock().nowMonotonic());
        L.wait(5);
    }
}

void AppController::analysisBody(Loop& L) {
    while (!L.threadShouldExit()) {
        const std::size_t n = analysis_.step();
        hb_.beat(ThreadId::Analysis, clock().nowMonotonic());
        // Deviation from the spec: a 2 ms poll instead of a counting semaphore, because the audio thread must not
        // signal a lock-based primitive. A real-time safe wake-up can replace this later without API change.
        if (n == 0) L.wait(2);
    }
}

void AppController::renderBody(Loop& L) {
    ss::Ns next = clock().nowMonotonic();
    while (!L.threadShouldExit()) {
        const double fps = render_.targetFps();
        const ss::Ns period = static_cast<ss::Ns>(1e9 / (fps > 1.0 ? fps : 25.0));
        render_.renderFrame();
        hb_.beat(ThreadId::Render, clock().nowMonotonic());
        next += period;                                    // schedule from elapsed time, never per-frame increments of state
        const ss::Ns now = clock().nowMonotonic();
        if (next < now - 4 * period) next = now;           // far behind: resync instead of bursting
        const ss::Ns wait = next - now;
        L.wait(wait > 0 ? static_cast<int>(wait / ss::kNsPerMs) + 1 : 1);
    }
}

void AppController::lightBody(Loop& L) {
    while (!L.threadShouldExit()) {
        lighting_.tick();
        hb_.beat(ThreadId::Light, clock().nowMonotonic());
        L.wait(1000 / lighting_.rateHz());
    }
}

void AppController::midiBody(Loop& L) {
    while (!L.threadShouldExit()) {
        midi_.tick();
        hb_.beat(ThreadId::MidiOut, clock().nowMonotonic());
        L.wait(2);
    }
}

void AppController::executeCommand(const AppCommand& c) {
    CommandOutcome o;
    o.correlationId = c.correlationId; o.kind = c.kind;
    auto it = commandHandlers_.find(static_cast<std::uint16_t>(c.kind));
    if (it == commandHandlers_.end()) {
        o.ok = false; o.message = "Not available in this build";
    } else {
        ss::Status s = it->second(c);
        o.ok = s.ok();
        if (!s.ok()) o.message = s.error().detail;
    }
    ui_.postCommandOutcome(o);
}

void AppController::ioBody(Loop& L) {
    std::vector<DiagEvent> ev;
    while (!L.threadShouldExit()) {
        AppCommand c;
        for (int budget = 16; budget > 0 && ui_.popCommand(c); --budget) executeCommand(c);
        ev.clear();
        if (diag_.drain(ev) > 0) {
            for (const auto& e : ev) {
                if (e.tag == "marker") journal_.addMarker(e.text);
                else if (e.tag == "scene") {
                    const std::uint32_t sid = static_cast<std::uint32_t>(std::strtoul(e.text.c_str(), nullptr, 10));
                    const auto sp = e.text.find(' ');
                    journal_.append(ss::diag::EventKind::Scene, sp == std::string::npos ? e.text : e.text.substr(sp + 1), sid);
                }
                else if (e.tag == "mode") journal_.append(ss::diag::EventKind::Mode, e.text);
                else if (e.tag == "adapt") journal_.append(ss::diag::EventKind::Adapt, e.text);
                else journal_.append(e.level == DiagLevel::Error ? ss::diag::EventKind::Error : ss::diag::EventKind::Note, e.tag + " " + e.text);
                const auto lvl = e.level == DiagLevel::Error ? LogLevel::Error : (e.level == DiagLevel::Warn ? LogLevel::Warn : LogLevel::Info);
                adapters_.log->write(lvl, e.tag + " " + e.text);
            }
        }
        shellIoTick(clock().nowMonotonic());        // dirty detection, 30 s autosave, device caches, learn polling
        hb_.beat(ThreadId::Io, clock().nowMonotonic());
        L.wait(20);
    }
}

}  // namespace ss::app
